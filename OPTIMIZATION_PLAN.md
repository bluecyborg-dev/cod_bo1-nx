# BO1 Switch port: optimization plan

The checklist of performance and stability work for the Switch port, in the order we follow. What each step found,
and the research behind the list, is in `docs/OPTIMIZATION_LOG.md` (progress log, discoveries, other ports, general
Switch techniques); deeper evidence is in `PORTING_RESEARCH.md` (§8c native renderer, §8d optimization research) and
`docs/OTHER_PORTS_RESEARCH.md`. When a step is tested, add its result to the log and tick it here.

Priorities (user): a stable frame rate first, the game looking good second; small flicker is acceptable; the chalk
weapon outlines are the lowest priority. Shadows stay off by default.

Rules for every step (from nfsmw-nx and masseffect-nx `measuring.md`):

- Measure on the console at stock clocks. One change per run, behind a setting, compared back to back in one session
  (run-to-run noise is about 3 FPS).
- Report the share of frames over 33.3 / 50 / 100 ms and their range, not only the average FPS.
- For CPU work, know which thread is the limit (game main thread, render thread, ring thread, streaming) and count
  core-milliseconds per frame.
- A configuration file can hide a change: check in the log what is really running.

## Where we stand (2026-10-16, Kino der Toten)

| | Value |
|---|---|
| Main build | plain guest memory + Mesa direct-draws driver (both default now) |
| Shadows off | 20 FPS cap most of the time: 147-201 swaps per 10 s, avg 189; renderer CPU ~14-30 ms, GPU ~25-30 ms per frame |
| Shadows lite / medium / full | avg 145 / 168 / 150 swaps per 10 s; the GPU is the limit |
| What limits it | heavy moments with shadows off: the game threads; shadows on: the GPU (sun shadow map) |
| Stability | no crash or hang in four console sessions (6-16 min each) |

## Checklist

### Next

- [ ] **Console CPU profile in Kino** (SDK stack sampler: `--bo1_profile_stacks=true --bo1_profile_log=true` sends
  each 10 s report to the Wi-Fi log, no flag file on the SD card needed): which guest
  thread and function holds the heavy moments with shadows off (125-147 swaps per 10 s with the renderer idle) and
  the 0.8 s hitch. Check the corpse theory, streaming I/O and kernel waits.

### Later (noted, not now)

- [ ] **`min` closer to `off`**: with the shadow system on, the game still builds ~2,900 shadow-map caster draws per
  frame that `min` discards (Mac: 905k draws per 10 s vs 326k with `off`, 580k skipped). Shrink the sun shadow range
  (`sm_sunShadowScale` < 0.25, `sm_sunSampleSizeNear` < 0.25) so they are not generated.
- [ ] **In-session A/B switch** (user: not a priority, the logs are enough for now): a button combo that flips the setting under test (like − + D-pad for shadows), with
  the log marking each 10 s window A or B, so one session in the same spot compares both instead of separate sessions
  in different rounds.
- [ ] **Hounds round** (first reached 2026-10-16): 124-175 swaps per 10 s, ~1,000-1,800 draws per frame (the hounds'
  fire, lightning and spawn effects), renderer CPU up to 51 ms and GPU up to 39 ms. Options: fewer effect draws through
  dvars (`fx_cull_effect_spawn` 1, `fx_drawClouds`, `r_glow`, `ai_corpseCount`; docs/deep-research-report-2.md), or the general GPU / renderer work below. The user wants it on the map, nothing done yet.

### GPU

- [x] ~~Skip the game's depth prepass~~: tested 2026-10-16, GPU 36 → 42 ms per frame (hidden pixels no longer
  rejected early) and less smooth; `bo1_native_skip_prepass` now off by default. Maybe worth another try with ZCULL on.
- [ ] 960x544 internal resolution (16-px tile grid).
- [ ] 30 FPS cap (2 vblanks) once ~90 % of frames fit in 31 ms.
- [ ] One vs two frames in flight (input lag felt with shadows on; ChatGPT report #5).
- [ ] ZCULL: works on the console (NVK gives every depth target a plane with `--bo1_native_zcull=true`), but 2026-10-16 showed no measurable gain and the user felt more stutters, slower textures and ZL acting like taps (cause unknown). Off by default; retry only with the in-session A/B switch.
- [ ] MSAA off (`bo1_native_msaa=false`, exists): the scene is 2x MSAA at 1040x608; one sample saves much GPU
  bandwidth, edges get rougher. Console A/B pending.
- [ ] Fewer passes and barriers (each is a wait for idle on NVK); skip resolves nobody reads.
- [ ] Depth-only pipelines without a fragment stage.

### Renderer thread (CPU per draw)

- [ ] **Hashing, compares and copies are ~40 % of the renderer's busy time** on the console (`XXH3_hashLong` 7.6 %,
  `memcmp` 7.5 %, `memmove` 7.4 % of its samples). Find the callers with the profiler's new pc/lr pairs (next
  sampling session), then cut the largest. Vertex uploads are ~1-1.7 MB per frame.
- [ ] Per-stage stopwatch on the renderer thread (PM4 decode, setup, textures, constants, uploads, Vulkan calls) and
  counters of requested versus emitted state changes (ChatGPT report #1/#2; Mass Effect's `C6 stages`).
- [ ] `bo1_native_record_thread` A/B (exists, off): NVK's ~4-5 µs per draw moves to a second thread, as Mass Effect's
  deferred recording does; it had no free core before the job queue fix.
- [ ] Back to ~12 µs per draw: sub-allocate texture memory from large blocks; upload only the mip levels a copy
  changed; create images off the ring.
- [ ] Render backend polls `WaitForSingleObject` with timeout 0 and yields on every timeout (seen in the Mac profile;
  confirm on the console first).

### Game threads and system

- [ ] Game busy-waits → real waits; thread priorities per thread from the profile (job queue workers busy in
  `sub_82545308` / `sub_825450A8` on the Mac).
- [ ] Range cache for the reread streaming archives (`images.pak`, `snd.all.pak`).
- [ ] `guest_memory_large_pages` A/B (modes 1 and 2).
- [ ] CPU boost mode during loading screens only.

### Faster guest code

- [ ] Direct calls instead of weak aliases.
- [ ] Float moves without FPCR mode switches (`fp_moves_modeless`).
- [ ] Non-volatile registers as locals / no LR stores (needs shared registers for setjmp/SEH).
- [ ] "Diet" lowerings.
- [ ] LTO + PGO, hot function ordering (experiment).
- [ ] Native memcpy.

### Longer term

- [ ] Mesa 26.2.4 for Horizon (mesa-switch PR #5) and its fixes (depth-stencil memory bindings, GPFIFO accounting).
  The 26.2.3 work (ZCULL, shader cache lock contention, redundant UBO updates) is already in our pin `d4a00ea0ab3`.
- [ ] Native replacements of the hottest game functions, with guards (needs the profile).
- [ ] Offline shader library.
- [ ] deko3d renderer instead of NVK.

### Visual issues (after performance)

- [ ] Sunlight flickers when firing (muzzle flash light).
- [ ] Occasional black textures while streaming.
- [ ] Oily rainbow swirl on the round lobby platform (gone with `r_stream 0` in the first test; recheck).
- [ ] Chalk weapon outlines turn into a huge white glow up close (lowest priority).
- [x] Characters and hands flicker lit/dark near lights: fixed with the shadow system on (`min`); returns with
  shadows off.

### Done

- [x] Native renderer, official 460.8 MHz handheld GPU profile, 3 work slots.
- [x] Fewer draws through game settings (`bo1_dvars`).
- [x] Page-indexed texture invalidation; placeholders while a texture builds.
- [x] Texture pool.
- [x] High-mip streaming off in game (`r_stream` written directly in its dvar).
- [x] Non-blocking logging; driver pipeline cache on the SD card.
- [x] SPIR-V cache on the SD card.
- [x] Pipeline list + prewarm at boot (slow frames from compiles 62 → 2 per session).
- [x] 20 FPS cap (`bo1_min_frame_vblanks` 3), held before presenting (`bo1_pace_before_present`, default on).
- [x] Live shadow presets (− + D-pad, settings menu) and the per-frame far-half skip.
- [x] PulseEvent without yield and sleep (Mac verified; in every console build since, not A/B'd alone).
- [x] Guest memory without volatile (`BO1_GUEST_PLAIN_MEMORY`, default on).
- [x] Mesa direct draws (`P8_DIRECT_DRAWS`, default on).
- [x] Main thread exempt from the job requeue throttle (it caused input lag); console 2026-10-16: best averages so far (175 swaps per 10 s, GPU 33 ms).
- [x] Network log sends from its own thread (a stuck `send()` froze the whole game for 1.7-4.4 s during Wi-Fi tests).
- [x] Job queue workers' busy wait throttled (`bo1_job_worker_sleep_us` 100): workers ~50-67 % → ~14-20 % CPU each.
- [x] Shadow presets cut to `off` (default) and `min` (shadow system on, no shadow map drawn: no flicker, ~15 % slower
  than off); lite, medium and full dropped (user, 2026-10-16).
- [x] Profiler reports in the Wi-Fi log (`--bo1_profile_log=true`; stack sampling `--bo1_profile_stacks=true`).
