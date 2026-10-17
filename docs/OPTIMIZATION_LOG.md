# BO1 Switch port: optimization log and discoveries

What we measured and learned while optimizing the Switch port: every build and setting tried on the console, the
findings behind them, snapshots of where we stood, the research on other ports and general Switch techniques. The
plan itself, a checklist, is `OPTIMIZATION_PLAN.md`. Add new results to the progress log (newest at the bottom) and
tick the matching checklist item there.

## 1. Progress log

| Build / setting | Result (Kino, console unless noted) |
|---|---|
| Page-indexed texture invalidation | removed millions of comparisons a frame (the 60 µs/draw regression) |
| Texture pool (from nfsmw-nx) | images created on the ring 15-30 → 5-12 ms per frame; worst pause 1.4 s → ~0.4 s; video 15-19 FPS |
| `r_stream 0` (no high-mip streaming) | first minute: renderer 19-27 ms, ~0 frames over 100 ms, video 20.1 FPS; rainbow floor gone. Opening a door (new area) brings the copies and texture creation back (16-44 copies/frame); video 17.7 FPS |
| `r_stream 0` in the dvars, re-applied every 5 s | **froze on the loading screen** (console and Mac): with streaming off a map load waits forever for the streamer, and rewriting it during a load restarts it |
| `bo1_stream_off_in_game` via `set r_stream` | froze again on the console: BO1's 3D main menu (~1,500 draws) looked like gameplay, streaming went off in the menu, and `set r_stream 1` at the loading screen never ran (the command buffer does not run during a load) |
| r_stream written **directly** in its dvar (`0x83CF6850` → dvar, current value at +0x18, latched +0x28; int 0..7, default 7) | loads work (menu off → load → restored → loaded), Mac and console. Docked: 179-200 swaps per 10 s at the 20 FPS cap, renderer 15-25 ms, GPU 15-20 ms; the slow frames left line up with shader and pipeline compiles (~509 shaders, ~436 pipelines a session) |
| **SPIR-V cache** on the SD card (2nd console session) | 462 of 511 shaders from the cache (38 MB loaded at boot, ~0.7 s); slow frames during a compile 62 → 21 per session, the rest mostly pipelines. The 77-swap window was the pause menu (~620 draws, GPU ~40 ms each) |
| SPIR-V cache design (Mac test) | Stored on the SD card (`cache/shaders/driver/native-*.spirv`, next to the driver cache): every translation is appended with its texture/sampler bindings and a checksum, and later sessions skip translating. Mac: 2nd run took 228 of 235 shaders from the cache, same draw counts; ~55 KB per shader (13 MB for Kino on the Mac) |
| **Pipeline list + prewarm**: every pipeline key is appended to `native-*.pipelines`; at boot they are compiled on the worker (draw requests first) with modules from the SPIR-V cache | console: 510 pipelines prewarmed in 766 ms at boot; slow frames during a compile per session 62 (no caches) → 21 (SPIR-V cache) → 14 (list being written) → **2** (prewarm); 491 of 496 shaders cached; play 170-200 swaps per 10 s |
| **Live shadow presets** (2026-10-15 evening): `bo1_shadows` off/lite/medium/full changes in game from the settings menu (L+R+Right) or with − + D-pad Right/Left; every preset sets all its dvars, and `sm_sunShadowScale` (applied by "set" only at the next map load) is also written straight into its dvar. Far-half skip now per frame (draws into base 1216 while the 384 map waits for its resolve), so the spot light map that reuses 1216 after the resolve is drawn | Mac: off ~305k draws/10 s, lite ~330k, full ~450k (+ ~400k skipped far-half draws); switching applies within 0.5 s. Default stays `off` (user's choice) |
| Shadow presets on the console (2026-10-16, both builds) | swaps per 10 s: off 192-200, lite 137-171, medium 143-147, full 107-163 (one 1.6 s hitch). Per frame, lite vs off: 1,366 vs 627 draws drawn (+ ~1,000 far-half draws skipped), renderer CPU 45-60 vs 18-25 ms, GPU 36-41 vs 28 ms. The presets barely differ because they all keep the sun shadow map (this build ignores `sm_sunEnable`). With shadows on the renderer thread needs more than the 50 ms budget, and the user felt **input lag**. Shadows on cost renderer-thread CPU per draw, which plain guest memory does not touch |
| **PulseEvent without yield + 10 µs sleep** (`event_pulse_fast`, SDK) | Mac sample: BO1's main thread spent ~70 % in NtPulseEvent (its job queue pulses per job); after: mostly a real wait. Console: pending |
| **Guest memory without volatile** (`BO1_GUEST_PLAIN_MEMORY`, `tools/codegen_post.py`, 13,048 loop barriers) | Mac: Kino runs normally. Console 2026-10-16 (6.5 min of Kino, no hang): the user found play "more fluid and smooth" than `out/nx`. The logs barely show it, since both sit at the 20 FPS cap with shadows off (192-200 swaps per 10 s, renderer CPU ~18-30 ms, GPU ~28 ms in both); the gain is on the game threads, not the renderer thread |
| **Mesa direct draws** (`P8_DIRECT_DRAWS=1`, project8-sw patch 0003 merged by hand: draws skip the draw macro when their parameters are zero; memory query cached) | driver built 2026-10-16 in `out/mesa-sdk-direct`; NRO `out/nx-direct` = `out/nx` relinked against it (APFS clone of the build dir, only the link step retargeted). Console 2026-10-16 (9 min of Kino, no GPU errors): the user's favourite build with shadows on. Renderer CPU per drawn draw roughly halved with shadows (lite ~1,430 draws/frame in 32 ms vs ~1,370 in 56 ms on `out/nx`); GPU unchanged (lite 36-40 ms), so lite/medium now sit at ~150-175 swaps per 10 s and the GPU is the next limit. Shadows off felt less smooth than `out/nx-fast`: in heavy scenes with shadows off (125-143 swaps) the renderer had headroom (26 ms CPU, 33 ms GPU), so the game threads were the limit, which plain guest memory addresses. Combined build `out/nx-fast-direct` (fast + this driver) sent for testing |
| **Plain guest memory + Mesa direct draws** (`out/nx-fast-direct`, 2026-10-16; now the default: `BO1_GUEST_PLAIN_MEMORY` ON, `P8_DIRECT_DRAWS` on, `build_nro.sh` uses `out/mesa-sdk-direct`) | console, 14 min of Kino, no crash: **best build overall** for the user, with and without shadows (prefers playing without). Swaps per 10 s: off 147-201, avg 189, 37 of 55 windows at 190+; lite 108-170 (avg 145); medium 154-175 (avg 168); full 132-183 (avg 150). One 0.8 s hitch with shadows off |
| **Profiler reports through `open_memstream`** (2026-10-16) | **froze the game 8 s after start, three times** (the moment the SDK profiler writes its first report, which it does in every session). Back to writing `rex_profile.log` as before and reading the new part back for the log: runs normally. Don't use `open_memstream` on Horizon |
| **Shadow preset `min`**: lite's dvars (shadow system on) + every shadow map draw skipped in the renderer (depth-only, no MSAA; skip reason 15) | console, 10 min: swaps per 10 s off 182-201 (avg ~195), min 119-190 (avg ~165). With the system on the game draws ~900-1,400 draws a frame against ~500-700 with it off, and those are not shadow maps, so skipping the maps helps less than hoped (lite averaged ~145). The user keeps only `off` and `min` |
| **Profiler, shadows off at the 20 FPS cap** (passive report, no stack sampling) | CPU total 300 % (all three game cores busy). JQ Worker 1 and 2 (guest entry 0x82545888) ~67 % each, GPU ring 35 %, Sound Mix 33 %, Main 24 %, Backend 23 %, Server 21 %, XMA decoder 19 %. Some slow frames wait ~50 ms for the GPU. The job workers look like spin-waiting: next, stack sampling |
| **Stack sampling on the console** (`--bo1_profile_stacks=true`, 2026-10-16, 6 min of Kino, no freeze) | JQ Worker 1/2 average ~50 % CPU each; 43 % of their samples in `sub_825450A8` (hand out a job: scans 8 priority lists, yields when empty) and `sub_82545308` (the worker loop), plus guest clock reads and mutex try-locks. The idle wait `sub_825444B8` only blocks (event, 1 ms timeouts) while the pending count is 0; with a job still running elsewhere it yields and returns, so the worker spins. Main thread: 26 % CPU, 67 % of its samples waiting in the kernel |
| **Job worker sleep** (`bo1_job_worker_sleep_us` 100, hook on `sub_825450A8`: after 16 empty answers in a row, sleep 100 us per further empty answer) | Mac: Kino normal (200 swaps per 10 s); the workers rarely idle there. Console (7.5 min of Kino, first hounds round): the workers still use 42-67 % each and the cores stay at 300 %, so the sleep almost never runs; the user felt it "maybe a bit more fluid, not sure". Hounds round: 124-175 swaps per 10 s, 1,000-1,800 draws per frame, renderer CPU 31-51 ms, GPU 32-39 ms |
| **Job requeue throttle** (`bo1_job_worker_sleep_us` 100, hook on `sub_82546470` when called from the worker loop's requeue at 0x8254551C: from the 16th tight requeue in a row, sleep 100 us) | The busy wait: a job whose callback returns "not finished" goes back on its list and is handed out again at once. Two persistent jobs do it, callbacks `sub_825BBFD0` / `sub_825BC040` (both return 1 while `sub_825BBB88()` is non-zero). Mac: ~16,000 requeues per second, ~15,000 now followed by a sleep; Kino normal (200 swaps per 10 s, same draws). Console (6 min of Kino, 2026-10-16): **JQ Worker 1/2 from ~50-67 % to ~14-20 % CPU each**; the cores no longer sit at 300 % (240-298 %), and the main thread gets more time (29-42 % vs 23-31 %). ~13,000 requeues per second, ~5,000 followed by a sleep. Shadows-off swaps per 10 s: avg 186, 50 % of windows at 190+, 5 % under 160, min 142 (previous session: avg 178, 42 %, 20 %, min 124, but it had the hounds round; sessions differ too much to rank by frame rate). User: "a bit smoother", and the green glow of the power-ups no longer drops the frame rate |
| **Depth prepass skip** (`bo1_native_skip_prepass`, default on; skip reason 16: depth-only draws that write depth into a multisampled target) | Frame trace (Mac): the scene's depth buffer gets 661 depth-only draws (GEQUAL, depth written: BO1's depth is reversed), then the colour pass draws 680, 669 of them testing GEQUAL **and writing depth**, so no state change is needed when the prepass goes. Mac: ~675 draws skipped per frame, Kino normal at 200 swaps per 10 s. **Console (11 min of Kino, 2026-10-16 15:34, same build as the helper throttle): worse.** No visual problems (user: "the same or a bit better"), renderer draws 987 → 559 per frame, but GPU time 36 → 42 ms per frame, and the ring thread spent 36 % of its time waiting on fences in BeginRecording (the GPU is the limit; "renderer CPU" 43 → 53 ms includes those waits). Without the prepass the colour pass shades hidden pixels that the prepass's depth used to reject early. Swaps per 10 s about the same (avg 166 vs 167-170), but 1,824 slow frames vs 1,274, and the user felt it "less smooth with more input lag". Default turned off |
| **Helper requeue throttle** (same hook, also for `sub_82545618`'s requeue from 0x8254578C) | Console (previous session): with only the workers throttled, Backend went from ~20-28 % to ~50-77 % of a core (it runs queued jobs while it waits). Mac: ~30,000 requeues per second counted, ~23,000 followed by a sleep; Kino normal. Console (2026-10-16 15:34): Backend 61 → 29 %, workers 16 %, but **Main XThread 30 → 49 %, 19 % of its time sleeping in this hook**: the main thread also runs "run one job" (`sub_8238FB98` → `sub_82545D98` → `sub_82545618`) while it waits for jobs, so the sleeps delayed the game thread itself (input lag). Fixed: the main thread (`XThread::main_thread()`) is never throttled |
| **ZCULL-capable depth targets** (`bo1_native_zcull`, off by default; same build) | NVK (our pin has the Mesa 26.2.3 Horizon ZCULL work) refused a ZCULL plane to every depth target because they were created with TRANSFER_DST. Now, with the option: depth targets at the full height of their EDRAM range (no growth copies), cleared by a clearing render pass. Mac: the scene depth target (1040x608, 2x MSAA, tile 1060) is created "ZCULL-capable" first time, Kino normal. Console (2026-10-16 17:39, ~5 min of Kino, build 20261016d): **NVK gave all five depth targets a ZCULL plane** (scene 1040x608 2x MSAA, sun shadow 520x1024, etc.; no errors). No clear gain: GPU 30.4 ms per frame at 968 draws vs 29.4 ms at 861 draws (previous 9-min session), swaps per 10 s 174 vs 186, slow frames per minute the same (71). Heavier scenes this time (more draws), so maybe a few %, within session noise. The frames over 400 ms were all ring-idle (the game not submitting), as in earlier sessions. User: more stutters, some textures (power-ups) took longer to appear, and holding ZL (aim) acted like rapid tapping. Slow SD reads per minute were the same as before (2.0 vs 2.2), the console was in handheld mode and the input code did not change, so the logs explain neither; ZCULL stays off by default. Next session (2026-10-16 18:02, same build without ZCULL): ZL holds normally again, so **ZCULL caused the ZL problem** (mechanism unknown); still some stutters |
| **What the remaining stutters are** (9.4-min session 15:55) | Slow frames: 646 of 50-59 ms (pacing, normal at the 20 FPS cap), 247 of 60-99 ms, 43 of 100 ms or more. The 70-199 ms frames average ~1,370 draws vs ~860 for an average frame, with 23 ms of GPU wait (95 of 217 over 30 ms), no shader or pipeline creation and ~2 texture uploads: heavy scenes (crowds, effects), not hitches from loading. Next: `bo1_fx_lite` (effect culling dvars) |
| **`bo1_fx_lite`** (build 20261016e: fx_cull_effect_spawn/elem_spawn/elem_draw 1, fx_drawClouds 0, ai_corpseCount 3; off by default) | Mac: the dvars are accepted, Kino normal (200 swaps per 10 s; no combat there to measure). Console (2026-10-16 18:13): no log; the game connected to a listener left over from the 18:02 run and blocked on stdout at a black screen until that listener was killed, then played with its network log dead. `tools/nxlink_run.sh` now stops old listeners first. Needs a session with the log |
| **Main thread exempt from the requeue throttle; prepass skip off** (build 20261016b) | Console (12 min of Kino, 2026-10-16 15:55): **best averages so far**: 175 swaps per 10 s (vs 166-170), renderer CPU 36 ms and GPU 33 ms per frame (vs 43-53 / 35-42), Main XThread 36 %, Backend 24 %, workers 17 %. User: "smoother than the previous build, less input lag", but "more heavy stutters": 18 frames over 400 ms, three of 1.7-4.4 s with the audio stopped too |
| **Whole-game freezes from the network log** (build 20261016d; the first try, with a std::thread sender, crashed at startup) | The stack sampler caught it: during the 4 s freezes the `[profile]` forwarding thread sat in `send()` inside `TcpLogSink` (MSG_DONTWAIT did not stop it blocking during a Wi-Fi / receiver stall) holding the sink's mutex, and the ring thread (44 %), Audio Worker, JQ Worker and the flusher all waited on that mutex. Test-only (log_network), but it is what the user felt as heavy stutters. Fixed: logging threads only queue the line (4 MB cap, drops counted); a sender thread does the send() calls. Console (2026-10-16 16:33, ~2.5 min of Kino): no whole-game freezes (the only frames over 400 ms are ring-idle loading waits), 0 lines dropped. User: "smooth, but with more stutters; no heavy stutters". In the same window (20-200 s after `devmap`) the previous session had 216 slow frames / 8 over 100 ms, this one 168 / 4: the extra stutters felt are within session noise |
| **Profiler pc/lr pairs** (SDK profiler, `pair image+0xPC <- image+0xLR` per active thread) | For leaf functions lr is the real caller; the next sampling session shows who calls the hashing, compares and copies on the renderer thread |

Even without churn a frame is ~50 ms (renderer ~25 ms, GPU ~30 ms as reported ≈ 48 ms real): 20 FPS is the stable
target today; 30 needs the GPU and the ring ~40 % lighter (depth prepass skip, 960x544). BO1 has no dvar for the
prepass, so skipping it is renderer work.

## 2. Discoveries

### Visual issues as first described (play and the videos in `gameplay/`, 2026-10-15)

- The chalk weapon outlines on walls turn into a huge blurry white glow up close (fine from a distance): the glow
  effect behind them (bloom, sprite scale or exposure).
- The round platform in the lobby shows an oily rainbow swirl: a reflection (cube) texture or its mips read wrong.
- Sunlight flickers when firing (muzzle flash light).
- Characters and the hands flickered between lit and dark when crossing between dark and lit areas (video 18:00:
  hand brightness 76/26/54/62/25/53 on consecutive frames, bursts of 1-4 s). Cause: `sm_enable 0`. With the shadow
  system on the flicker is gone (console, 3 sessions). Full shadows cost 11-15 fps, so `bo1_shadows=lite` keeps the
  system on but draws almost nothing: one spot light, world casters only, sun shadow range x0.25. In this build
  `sm_sunEnable`, `sm_spotEnable` and `r_spotLightShadows` do nothing; `sm_maxLights` and the sun scale dvars work.
  The sun shadow map is 1024x2048 drawn in two halves at EDRAM bases 384 and 1216 (inside the first) and resolved
  from 384: our separate image per base meant the far half (~2,300 draws, ~620 with lite) was never resolved;
  `bo1_native_skip_aliased_depth` skips it. Mac draws per 10 s: off 310k, full ~890k, lite 340k.
- Occasional black textures while streaming.

### What limits the frame rate (console logs, 2026-10-16)

- **Shadows off:** the game holds the 20 FPS cap most of the time (renderer CPU ~14-30 ms, GPU ~25-30 ms per frame
  as NVK reports). The heavy moments that still drop to ~125-147 swaps per 10 s leave the renderer and the GPU with
  spare time, so the limit there is on the game threads (plain guest memory helped exactly this; a console CPU
  profile is the next step).
- **Shadows on:** every preset keeps the sun shadow map, which roughly doubles the draws per frame (627 → ~1,370) and
  is most of the cost; that is why lite, medium and full look and perform alike. Before the direct-draws driver the
  renderer thread needed 45-65 ms per frame, over the 50 ms budget, and play felt laggy (each input waits behind
  frames the renderer has not caught up with). With the driver the renderer CPU per draw roughly halved; the GPU
  (36-51 ms per frame) is now the limit with shadows on.
- **Ranges between runs** are loose comparisons: rounds, zombie counts and spots differ.

## 3. Snapshots

### Where we stood early on 2026-10-15 (Kino der Toten)

| Measure | Value |
|---|---|
| Frame rate | ~10-12 FPS average, unstable (most frames over 50 ms; 30-50 % over 100 ms) |
| Draws per frame | ~600-1,350 (was ~4,800 before dynamic shadows were turned off) |
| Ring thread (renderer) CPU | 55-90 ms per frame; **texture lookups 25-52 ms** of it (1.5-3 images created per frame on the ring; the same large textures re-uploaded up to 7 times in 10 s) |
| GPU (as NVK reports; real ≈ ×1.6) | ~25-33 ms per frame |
| Game side | stalls of 140-600 ms with the renderer idle; feels worse after a while (more zombies/corpses?) |
| SD card | 280 slow reads per session (3.4 s, worst 45 ms), many rereads of `images.pak` / `snd.all.pak` |
| Image | correct; minor flicker left (sunlight flickers when firing); some textures black for a moment |

### Frame rate from the gameplay videos

Frame rate measured from the videos (distinct frames of a 30 FPS capture, build before the texture pool):
20.9 / 16.0 / 16.1 FPS; median frame gap 33-67 ms, 90th percentile 67-100 ms, 23-61 gaps over 100 ms per 30 s,
worst 1.4 s.

## 4. What paid off in the other ports

| Technique | Who | Result there | BO1 status |
|---|---|---|---|
| Native renderer, official GPU 460.8 MHz handheld profile, 3 work slots, async output | all | 1-3 → 16-29 FPS | done |
| Fewer draws through game settings (dynamic shadows, glow, DOF, LOD) | CoD4 port | — | done (`bo1_dvars`) |
| Skip the game's depth prepass | ME, CoD4 | ~1/3 fewer scene draws, artefacts fixed | **todo** (~675 draws/frame in BO1) |
| Lower internal resolution on the 16-px tile grid (960x544) | ME, NFS:MW | ME +50 %, NFS 28 → 35 FPS | **todo** |
| Ring CPU per draw: caches, no per-draw allocation/logging, upload dedupe, deferred recording | NFS:MW, ME | 9-21 µs per draw | **in progress** (ours 55-90 µs) |
| Texture memory sub-allocated from big blocks (no kernel round trip per texture) | NFS:MW | — | **todo** (each texture = one allocation now) |
| Frame pacing: fixed swap interval once ~90 % of frames fit | NFS:MW, project8 | removes cadence jitter | **todo** |
| Game busy-waits → real waits; thread priorities (render thread up) | all; ME +1.8 / +1 FPS | +5-10 % | partly |
| Codegen: direct calls, non-volatile registers as locals, "diet" ops | NFS:MW, ME | +4 FPS / +9 % / -10 % main thread | **todo** |
| Guest memory accesses without `volatile` | ME | +15-20 % | done (in our SDK) |
| LTO / PGO / function ordering | NFS:MW (yes), ME (no gain) | mixed | later experiment |
| Range cache for reread streaming archives | ME | stutters correlated with rereads | **todo** |
| Pipeline prewarm list + driver cache on SD | ME, project8 | cold hitches of 1-5 s removed | partly (driver cache on) |
| Native replacements of the hottest game functions, with guards | NFS:MW, ME | +1.4 FPS, -70 % of 60 ms frames | later (needs a profile) |
| Offline shader library (XenosRecomp + DXC) | NFS:MW, ME | leaner shaders, no runtime compiles | later (big job) |
| Newer Mesa for Horizon (26.2.3: ZCULL, less lock contention, fewer UBO updates) | mesa-switch | lower per-draw CPU | later |
| deko3d renderer instead of NVK | nfsmw-nx branch | frames ≥100 ms 4.5 → 1.0/min | long term |

Did **not** pay off elsewhere: lower resolution while CPU bound, resolutions off the 16-px grid, batching barriers,
arguments in registers on top of locals, more swapchain images, a separate presentation thread (higher average,
3x wider frame-time range), overclocking as the target.

## 5. General Switch optimization techniques

Collected for a stable port: hardware facts that decide what is fast, and the techniques commercial and homebrew
ports use. Sources at the end. "BO1:" notes say how each applies here.

### Hardware facts that matter

| Part | Fact | Consequence |
|---|---|---|
| CPU | 4× Cortex-A57 at 1020 MHz (same docked and handheld); a game gets 3 cores (0-2), core 3 is the system's | budget ≈ 100 core-ms per frame at 30 FPS; single-thread speed is low |
| CPU caches | L1D 32 KB (2-way), L1I 48 KB, **L2 2 MB shared** by all cores (~22 cycles), DRAM ~250 cycles | working sets beyond 2 MB thrash; all threads compete for L2 |
| CPU TLB | 32-entry L1 DTLB, 1024-entry L2 TLB (4 MB reach with 4 KB pages), slow misses | **2 MB pages matter**; scattered accesses over large memory cost TLB misses |
| CPU execution | 40-bundle reorder window, small schedulers, 12-entry store queue, 1 FMA/cycle (10-cycle latency), multiplies 5 cycles | dependent chains and big loops of scattered loads are slow; straight-line, cache-friendly code wins |
| Memory | 4 GB LPDDR4 shared by CPU and GPU, 25.6 GB/s theoretical, **~8 GB/s achieved by one CPU core**; 1331.2 MHz handheld, 1600 docked | every copy (vertex uploads, untiling, memcpy) competes with the GPU for bandwidth |
| GPU | Maxwell GM20B, 2 SMs / 256 lanes; 307.2 MHz handheld default, **460.8 MHz with the official profile**, 768 docked | the GPU is ~2.5× weaker handheld than docked |
| GPU caches | L1 12 KB per SM pair, **L2 256 KB**, DRAM latency > 400 ns | bandwidth-heavy passes (full-screen post, big render targets, MSAA) are expensive |
| GPU features | packed FP16 (2× rate where the compiler can use it), ZCULL + early-Z (lost if a shader writes depth or uses kill/alpha test carelessly), no stencil export from shaders | keep depth writes in fixed function; FP16 in shaders is a possible later gain |
| Storage | SD card through UHS-I, ~60-95 MB/s sequential; A2-rated cards load 15-25 % faster in streaming games | random small reads and rereads hurt; card quality matters |
| Process | ~3189 MB with title takeover / forwarder (album applet ~400 MB: unusable) | always launch with full memory |

### CPU and threads

- **Only priority 0x3B is time-sliced; everything else is cooperative.** A busy thread at another priority keeps its
  core, even while other cores are idle. Recompiled games busy-wait, so guest threads run at 0x3B and host service
  threads (audio 0x2B, presentation 0x2C, ring 0x2D) above them. When a thread starves, lower the hog instead of
  raising the starving one. BO1: SDK already does this; tune per thread with profiles.
- **A preferred core is not a pin** (0.24 migrations per loop measured); an exclusive core mask pins. Pinning the main
  thread alone on a core helped ME (`exclusive_core 32`: main thread on core 2, render thread raised to 0x2B) but
  lowered NFS:MW's minimum (16.3 vs 21 FPS). A/B per game.
- **Replace the game's busy-waits with real waits** (hook the polling function, sleep with a timeout or until woken,
  then run the original). On the 360 a spinning hardware thread is nearly free; here it steals one of three cores.
- **Kernel calls are expensive**: every SVC/IPC (signalling a condition variable even with no waiter, fence queries,
  mapping memory) costs microseconds. ME found one guest timer thread at 92 % of a core from `notify_all` per release,
  mallocs per wait and `dynamic_cast` per handle. Avoid per-frame/per-draw syscalls and allocations.
- **Never read `cntvct_el0`** (faults); use `armGetSystemTick`. `std::thread::detach()` can throw; use persistent
  workers. Decide to sleep or wake only under the lock (lost wake-ups hung NFS:MW's copy thread).
- **Logging costs**: synchronous logging to the SD card from hot threads stalls them (ME: 14 % of the ring thread in
  slow frames); use a non-blocking ring (`log_nonblocking`) and no logging from the ring thread. BO1: on.
- **Codegen**: direct calls instead of weak aliases, registers as C++ locals, "diet" lowerings, no `volatile` guest
  memory, `-ffp-contract=off` for exactness. LTO/PGO and function ordering helped NFS:MW (code locality matters with a
  48 KB I-cache), not ME.
- **Native replacements** of the hottest guest functions (with a guard that compares against the original before
  trusting it) are the last-mile CPU win.

### Memory

- Commit guest memory in **2 MB granules** (kernel memory block limit) and map it so views are **2 MB-congruent**
  (`guest_memory_large_pages`): the A57's TLB is small. The CoD4 port enables it (worst 30 s window 33.8 → 35.3-36.5
  FPS in its heaviest scene). BO1: available, off; A/B mode 1 and 2.
- **Sub-allocate GPU memory** from large blocks (16-32 MB) instead of one `vkAllocateMemory` per texture: each
  allocation and free is a kernel round trip (nvmap), and their number is limited. Free on a background thread.
- Keep a **texture memory budget** with LRU eviction by time, not frames. Cap descriptor/pipeline caches.
- Avoid emulated memory accesses (page watching): each fault is expensive and the exception path loses vector
  registers when preempted. BO1: native renderer, nothing watched.

### GPU

- Use the **official 460.8 MHz handheld profile** (`apm` configuration 0x92220008, memory left at 1331.2); docked needs
  nothing. Don't raise memory to 1600 by accident (0x92220007) unless measured.
- **Resolution is the biggest GPU lever**: render at 960x544 (multiple of the 16-px tile height) or lower and scale;
  commercial ports use dynamic resolution (Wolfenstein II down to 640x360, Doom variable). Off-grid sizes lose fast
  paths. Lower resolution does nothing while CPU bound.
- **Cut passes, not pixels, first**: shadow cascades, depth prepass, reflections, DOF, distortion, glow, MSAA.
- **Keep early-Z/ZCULL**: no shader depth writes, careful with kill/alpha test; draw the sky late.
- **Vertex work**: write only the outputs the pixel shader reads (ME: scene 66 → 36.6 ms); position `invariant` only.
- **Constants through dynamic uniform buffers**, inverse texture sizes as uniforms, merged predicated blocks in
  translated shaders, depth-only pipelines without a pixel shader.
- **Bandwidth**: swap images instead of copying on resolves, don't load attachments that are fully overwritten, avoid
  64-bit formats where 32 do, compressed textures stay compressed (BC on the GPU).
- **Pipelines**: precompile known pipelines at boot (prewarm list), keep the driver cache on the SD card, per-stage
  shader cache keys in NVK; compiling on the render thread causes 1-5 s hitches on a cold start.
- **API overhead**: NVK costs 9-21 µs per draw on the ring thread; deko3d (NVN-like) 8-14 µs with far fewer ≥100 ms
  frames. Newer Mesa for Horizon (26.2.3) reduces lock contention and redundant UBO updates.
- FP16 (packed) can double ALU throughput where precision allows; compiler support is limited.

### Storage and streaming

- The SD card is slow for small random reads; streaming games reread the same ranges. Cache reread ranges in RAM
  (ME's range cache), read in larger aligned chunks, keep the I/O thread separate from the game thread, and never
  block the render or game thread on a read.
- A good A2 card helps; a failing card causes silent corruption (BO1 history: quarantine + on-console hashing).

### Clocks and power

- Stock clocks are the target; overclocking hides the real limit (all ports).
- **CPU boost mode** (`appletSetCpuBoostMode`, CPU 1785 MHz) throttles the GPU to 76.8 MHz: unusable in play
  (ME: 3-4 FPS), but Nintendo's own games use it **during loading screens** (Zelda, Mario Odyssey, MK11). BO1: loading
  is CPU-bound (fastfile inflate, asset setup); boost during loads only, then back.
- Docked mode gives the GPU 768 MHz; the CPU is the same. Follow the effective mode (Reverse-NX can fake it).

### Presentation and frame pacing

- One Vulkan queue, three swapchain images, only FIFO and IMMEDIATE (IMMEDIATE does not tear: the compositor still
  composes at 60 Hz). Present from the thread that produces the image.
- **Steady beats fast**: frames are shown in 16.67 ms steps. Lock to a swap interval (2 = 30 FPS, 3 = 20 FPS) once ~90 %
  of frames fit the interval; a separate presentation thread raised the average but tripled the frame-time spread.
- Input lag grows with frames in flight and with low FPS; keep work slots at 3, don't queue more.

### Measuring

- Stack profiler (SDK: `logs/rex/stacks_profile.flag`), per-thread CPU, frame-time histogram (>33/50/100 ms), core-ms.
- NVK GPU timestamps read ~1.6× low; multiply by 1.627.
- One change per run, A/B in the same session, report the spread.

### Sources

- masseffect-nx and nfsmw-nx `docs/` (platform-notes, performance-history, measuring, streaming-io, kernel-waits,
  cold-start-hitches), project8-sw `docs/native-renderer.md`, the CoD4 port's `codmw.toml` (on the SD card).
- Chips and Cheese, "Cortex A57, Nintendo Switch's CPU": https://chipsandcheese.com/p/cortex-a57-nintendo-switchs-cpu
- Chips and Cheese, "Nintendo Switch's iGPU: Maxwell Nerfed Edition":
  https://chipsandcheese.com/p/nintendo-switchs-igpu-maxwell-nerfed-edition
- GDC 2019, "Witcher 3 on the Nintendo Switch: CPU & Memory Optimization" (Saber Interactive / NVIDIA; slides behind
  GDC Vault): https://gdcvault.com/play/1026635/-Witcher-3-on-the
- nfsmw-nx PR #27 (deko3d renderer): https://github.com/StevensND/nfsmw-nx/pull/27
- mesa-switch PR #4 (Mesa 26.2.3 for Horizon): https://github.com/danfromtico/mesa-switch/pull/4
- Phoronix, NVK ZCULL: https://www.phoronix.com/news/NVK-ZCULL-Merged
- Boost mode in loading screens (Digital Foundry via Nintendo Life):
  https://www.nintendolife.com/news/2019/05/digital_foundry_finds_mortal_kombat_11_also_using_switchs_boost_mode
- Wolfenstein II / Doom dynamic resolution on Switch: https://www.engadget.com/2018/06/30/wolfenstein-ii-switch-graphics-tech-comparison/
- devkitPro deko3d: https://github.com/devkitPro/deko3d
- Nintendo microSD guidance (UHS-I, 60-95 MB/s): https://nintendo.com/au/support/articles/which-microsd-cards-are-supported
