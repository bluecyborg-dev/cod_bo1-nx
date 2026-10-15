# BO1 Switch port: optimization plan

The working list of performance and stability work for the Switch port: where we stand, what the other Xbox 360 →
Switch ports learned, and the order we follow. Detailed evidence lives in `PORTING_RESEARCH.md` (§8c native renderer,
§8d optimization research); general Switch techniques are in the second half of this file.

Rules for every step (from nfsmw-nx and masseffect-nx `measuring.md`):

- Measure on the console at stock clocks. One change per run, behind a setting, compared back to back in one session
  (run-to-run noise is about 3 FPS).
- Report the share of frames over 33.3 / 50 / 100 ms and their range, not only the average FPS.
- For CPU work, know which thread is the limit (game main thread, render thread, ring thread, streaming) and count
  core-milliseconds per frame.
- A configuration file can hide a change: check in the log what is really running.

## Where we stand (2026-10-15, Kino der Toten)

| Measure | Value |
|---|---|
| Frame rate | ~10-12 FPS average, unstable (most frames over 50 ms; 30-50 % over 100 ms) |
| Draws per frame | ~600-1,350 (was ~4,800 before dynamic shadows were turned off) |
| Ring thread (renderer) CPU | 55-90 ms per frame; **texture lookups 25-52 ms** of it (1.5-3 images created per frame on the ring; the same large textures re-uploaded up to 7 times in 10 s) |
| GPU (as NVK reports; real ≈ ×1.6) | ~25-33 ms per frame |
| Game side | stalls of 140-600 ms with the renderer idle; feels worse after a while (more zombies/corpses?) |
| SD card | 280 slow reads per session (3.4 s, worst 45 ms), many rereads of `images.pak` / `snd.all.pak` |
| Image | correct; minor flicker left (sunlight flickers when firing); some textures black for a moment |

## What paid off in the other ports

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

## Progress log

| Build / setting | Result (Kino, console) |
|---|---|
| Page-indexed texture invalidation | removed millions of comparisons a frame (the 60 µs/draw regression) |
| Texture pool (from nfsmw-nx) | images created on the ring 15-30 → 5-12 ms per frame; worst pause 1.4 s → ~0.4 s; video 15-19 FPS |
| `r_stream 0` (no high-mip streaming) | first minute: renderer 19-27 ms, ~0 frames over 100 ms, video 20.1 FPS; rainbow floor gone. Opening a door (new area) brings the copies and texture creation back (16-44 copies/frame); video 17.7 FPS |
| `r_stream 0` in the dvars, re-applied every 5 s | **froze on the loading screen** (console and Mac): with streaming off a map load waits forever for the streamer, and rewriting it during a load restarts it |
| `bo1_stream_off_in_game` via `set r_stream` | froze again on the console: BO1's 3D main menu (~1,500 draws) looked like gameplay, streaming went off in the menu, and `set r_stream 1` at the loading screen never ran (the command buffer does not run during a load) |
| Now testing | r_stream written **directly** in its dvar (`0x83CF6850` → dvar, current value at +0x18, latched +0x28; int 0..7, default 7); the dvar's own value is the state. Mac: a load started with streaming off and stuck resumes as soon as 7 is written. Plus the 20 FPS cap |

Even without churn a frame is ~50 ms (renderer ~25 ms, GPU ~30 ms as reported ≈ 48 ms real): 20 FPS is the stable
target today; 30 needs the GPU and the ring ~40 % lighter (depth prepass skip, 960x544). BO1 has no dvar for the
prepass, so skipping it is renderer work.

## Order of work

1. **Ring thread back to ~12 µs per draw** (in progress). Done: page-indexed texture invalidation (was millions of
   comparisons a frame), placeholders while a texture builds. Measuring now: texture lookup time split into hashing
   and image creation. Next, depending on the result: sub-allocate texture memory from large blocks; upload only the
   mip levels a copy changed instead of the whole texture; move image creation back off the ring.
2. **Find the game-side stalls** with the SDK stack profiler (`logs/rex/stacks_profile.flag` next to the NRO): which
   guest thread and function; streaming I/O (range cache), kernel waits, thread priorities. Check the corpse theory.
3. **Skip the depth prepass**, then **960x544** internal resolution.
4. **Frame pacing**: a steady cap (likely 20 FPS = 3 vblanks) first; 30 when ~90 % of frames fit in 31 ms.
5. **Faster guest code**: direct calls first, then the diet ops; LTO/PGO as an experiment.
6. Longer term: newer Mesa, native hot functions, offline shader library, deko3d.

Known visual issues to revisit after performance (from play and the videos in `gameplay/`, 2026-10-15):

- The chalk weapon outlines on walls turn into a huge blurry white glow up close (fine from a distance): the glow
  effect behind them (bloom, sprite scale or exposure).
- The round platform in the lobby shows an oily rainbow swirl: a reflection (cube) texture or its mips read wrong.
- Sunlight flickers when firing (muzzle flash light).
- Occasional black textures while streaming.

Frame rate measured from the videos (distinct frames of a 30 FPS capture, build before the texture pool):
20.9 / 16.0 / 16.1 FPS; median frame gap 33-67 ms, 90th percentile 67-100 ms, 23-61 gaps over 100 ms per 30 s,
worst 1.4 s.

---

# General Switch optimization techniques

Collected for a stable port: hardware facts that decide what is fast, and the techniques commercial and homebrew
ports use. Sources at the end. "BO1:" notes say how each applies here.

## Hardware facts that matter

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

## CPU and threads

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

## Memory

- Commit guest memory in **2 MB granules** (kernel memory block limit) and map it so views are **2 MB-congruent**
  (`guest_memory_large_pages`): the A57's TLB is small. The CoD4 port enables it (worst 30 s window 33.8 → 35.3-36.5
  FPS in its heaviest scene). BO1: available, off; A/B mode 1 and 2.
- **Sub-allocate GPU memory** from large blocks (16-32 MB) instead of one `vkAllocateMemory` per texture: each
  allocation and free is a kernel round trip (nvmap), and their number is limited. Free on a background thread.
- Keep a **texture memory budget** with LRU eviction by time, not frames. Cap descriptor/pipeline caches.
- Avoid emulated memory accesses (page watching): each fault is expensive and the exception path loses vector
  registers when preempted. BO1: native renderer, nothing watched.

## GPU

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

## Storage and streaming

- The SD card is slow for small random reads; streaming games reread the same ranges. Cache reread ranges in RAM
  (ME's range cache), read in larger aligned chunks, keep the I/O thread separate from the game thread, and never
  block the render or game thread on a read.
- A good A2 card helps; a failing card causes silent corruption (BO1 history: quarantine + on-console hashing).

## Clocks and power

- Stock clocks are the target; overclocking hides the real limit (all ports).
- **CPU boost mode** (`appletSetCpuBoostMode`, CPU 1785 MHz) throttles the GPU to 76.8 MHz: unusable in play
  (ME: 3-4 FPS), but Nintendo's own games use it **during loading screens** (Zelda, Mario Odyssey, MK11). BO1: loading
  is CPU-bound (fastfile inflate, asset setup); boost during loads only, then back.
- Docked mode gives the GPU 768 MHz; the CPU is the same. Follow the effective mode (Reverse-NX can fake it).

## Presentation and frame pacing

- One Vulkan queue, three swapchain images, only FIFO and IMMEDIATE (IMMEDIATE does not tear: the compositor still
  composes at 60 Hz). Present from the thread that produces the image.
- **Steady beats fast**: frames are shown in 16.67 ms steps. Lock to a swap interval (2 = 30 FPS, 3 = 20 FPS) once ~90 %
  of frames fit the interval; a separate presentation thread raised the average but tripled the frame-time spread.
- Input lag grows with frames in flight and with low FPS; keep work slots at 3, don't queue more.

## Measuring

- Stack profiler (SDK: `logs/rex/stacks_profile.flag`), per-thread CPU, frame-time histogram (>33/50/100 ms), core-ms.
- NVK GPU timestamps read ~1.6× low; multiply by 1.627.
- One change per run, A/B in the same session, report the spread.

## Sources

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
