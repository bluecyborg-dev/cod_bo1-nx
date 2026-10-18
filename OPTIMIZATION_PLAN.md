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

### Recommended next steps (reordered 2026-10-17)

Ranked by expected gain per effort against the console evidence. These are candidates, not promised gains. Keep each
console comparison to one setting change, the same scene and logging, and report frame-time tails as well as FPS. Use
stock clocks unless the item explicitly tests an overclock interaction. The stock-clock fourth-core comparison was
inconclusive; the user has requested a separate overclocked retest.

1. [ ] **Deferred by user: `bo1_fx_lite` A/B in an effect-heavy Kino scene.** Keep the preset off for now; revisit
   only if we can compare a repeatable effect-heavy segment. The first toggle smoke pass lacked tagged profiler
   reports. A follow-up OFF → ON → OFF pass captured 9/13/9 complete reports, but draw counts and load varied
   substantially and the phase averages showed no gain (details in `docs/OPTIMIZATION_LOG.md`). This existing preset
   enables effect/element culling, disables cloud effects, and lowers `ai_corpseCount` to 3. Long frames were
   associated with about 1,370 draws versus about 860 in ordinary frames, without shader or pipeline creation; this
   is the most direct low-effort candidate for reducing CPU and GPU work in zombie/effect bursts. It may remove
   visible effects and corpses. Launch with `--bo1_profile_log=true` so the 10 s tagged reports reach the Wi-Fi log.
   Use the settings overlay (`L+R+Right`), category **BO1 A/B**, to switch it live in the same scene.
   Switch state after a profiler report, ignore the first block after each change because it can include both states,
   and collect at least three complete blocks per phase. An OFF → ON → OFF pass helps expose round-to-round drift.
   The profiler prints `A/B condition: bo1_fx_lite=ON/OFF`; transition logs
   record when the change's dvar batch was queued. The earlier console attempt lost its log because a stale listener
   blocked output; the launch script now stops stale listeners. Separate the dvars later only if the preset helps and
   we need to identify which one matters.
2. [x] **MSAA-off console attempt parked after a visual regression.** On 2026-10-18, the user reported a large black
   box over the main menu after launching with `bo1_native_msaa=false`. The log confirms 1x targets but identifies no
   specific renderer error; there is no gameplay performance result. Keep the normal 2x setting and revisit only
   after investigating the black-box artifact.
3. [x] **Overclocked Kino diagnostic captured (2026-10-18).** The archived build ran 90 ten-second map-stage reports
   with `--bo1_profile_log=true --bo1_native_hash_counters=true`; see `out/net_logs/20261018-023901.log` and the
   detailed result in `docs/OPTIMIZATION_LOG.md`. The clock and temperature readings stayed steady, and the measured
   hash/copy work was modest on average. The log also records nine post-load pauses of 1.7–2.6 s, almost entirely
   renderer-ring idle. This was an overclock diagnostic, not a stock-clock comparison, and stack sampling was off.
4. [ ] **Short stack diagnostic for long frames.** The overclocked log confirmed that the previous run launched with
   stack sampling disabled. It also exposed an unconnected profiler hook: the native renderer never marked long-frame
   intervals, so the profiler could not filter samples into the "during the hitches" section. The next build records
   frames over 100 ms and associates those windows with stack samples. Capture a short Kino segment with
   `--bo1_profile_log=true --bo1_profile_stacks=true`. This is diagnostic only: 1 kHz sampling pauses threads and
   can perturb pacing, so do not use it to judge performance.
5. [ ] **Overclocked fourth-core workload-mask A/B.** The user reopened core-3 testing after the overclock capture.
   Keep the same overclock settings and scene, then compare the same NRO with
   `--bo1_profile_workload_core_mask=7` and `=15`, plus `--bo1_profile_log=true`; leave stack sampling off for this
   performance comparison. Process permission stays `0xF`; only tracked workload threads are restricted in the `0x7`
   leg. Allow the map to settle and collect at least three complete 10 s reports per leg. Compare FPS, process CPU,
   frames over 100 ms, per-core system-busy readings, and the reported effective clocks. This tests whether the fourth
   core helps at the elevated clocks; it cannot repair Atmosphère's unsupported per-core `ThreadTickCount` accounting.
6. [ ] **Target the measured renderer CPU cost if it remains limiting.** Earlier console samples put hashing,
   compares, and copies at about 40% of renderer busy samples. The new counters show where those costs land per frame,
   but not their callers. After the stall source is understood, capture pc/lr callers and optimize the largest verified
   renderer cost; add per-stage timers and requested-versus-emitted state counters if call sites remain unclear. The
   geometry-hash cadence A/B did not show a gain, so do not repeat cadence changes without new evidence.
7. [ ] **Main XThread priority A/B (`0x3B` → `0x3A`) only if profiling shows the game main thread is limiting.** It
   could give the main thread more CPU, but its yielding job-wait loop may starve same-core `0x3B` workers. Keep the
   worker throttle enabled and check input latency, frame tails, and worker progress.
8. [ ] **Console-trained PGO after a repeatable route is established.** Instrumented builds dump/reset gcov counters
   to `sdmc:/pgo` every three minutes; rebuild with the same GCC and layout, and never benchmark the instrumented
   binary. PGO could improve hot guest/native code broadly, but needs a capture session and release rebuild. Add LTO
   only when Docker has enough memory.

### Completed or parked experiments

- [x] **Stock-clock fourth-core access and workload-mask A/B.** The process can run on core 3, but matched `0x7`/`0xF`
  runs showed no clear average gameplay gain. The user later reopened the question specifically at overclocked clocks;
  see the new A/B above. Wake-core histograms and affinity tuning remain deferred unless the overclocked comparison
  shows a gain. Full helper validation and stock-clock results are in `docs/OPTIMIZATION_LOG.md`.
- [x] **Geometry-hash cadence A/B.** No clear gain; leave the 16-frame setting off. Exact-hash follow-up is deferred.
- [x] **`bo1_native_depth24` A/B.** No clear gain and user saw more light flickering; leave it off.
- [x] **Depth-prepass skip and ZCULL.** Prepass skip raised GPU time from 36 to 42 ms/frame. ZCULL showed no
  measurable gain and had reported stutters/input behavior; both stay off. Revisit ZCULL only with a same-session A/B.

### Later (noted, not now)

- [ ] **`min` closer to `off`**: with the shadow system on, the game still builds ~2,900 shadow-map caster draws per
  frame that `min` discards (Mac: 905k draws per 10 s vs 326k with `off`, 580k skipped). Shrink the sun shadow range
  (`sm_sunShadowScale` < 0.25, `sm_sunSampleSizeNear` < 0.25) so they are not generated.
- [x] **Live-menu A/B for `bo1_fx_lite`.** Settings overlay (`L+R+Right`) can change the preset during a run; the
  profiler labels each 10 s report block with the active phase, and the app log records when transition batches
  queue. This avoids rebuilding/relaunching between FX phases. Other settings need their own live-safety review;
  MSAA requires restart.
- [ ] **Hounds round as a stress scene for the prioritized tests.** Earlier runs had 124-175 swaps per 10 s,
  ~1,000-1,800 draws per frame, renderer CPU up to 51 ms and GPU up to 39 ms. Use it for `bo1_fx_lite` and GPU A/Bs
  when it is practical to reach; compare the same workload and note visual changes.

### GPU

- [x] **MSAA off (`bo1_native_msaa=false`) parked:** the first console launch produced a large black box over the
  main menu, with no gameplay performance data. Keep 2x MSAA enabled while investigating the visual regression.
- [ ] One vs two frames in flight: compare throughput and frame tails against input latency (input lag was felt
  with shadows on; ChatGPT report #5).
- [ ] 30 FPS cap (2 vblanks) once ~90 % of frames fit in 31 ms.
- [ ] Count compute dispatches between draws and inventory passes/barriers/resolves. Then skip unused resolves,
  reduce unnecessary barriers, or move suitable work to a graphics pass; each draw/compute switch can idle NVK.
- [ ] Depth-only pipelines without a fragment stage, if captures show meaningful depth-only work.
- [x] **Depth-prepass skip tested and rejected:** GPU time rose from 36 to 42 ms/frame and gameplay felt less
  smooth; keep `bo1_native_skip_prepass` off.
- [x] **ZCULL tested and parked:** it worked, but showed no measurable gain and the user reported more stutters,
  slower textures, and held ZL acting like taps. Revisit only with a same-session A/B switch.
- [ ] Resolution reduction is not a ready console A/B: the handheld output is already 1280x720, while BO1 renders
  its scene at 1040x608. Raising the internal target to 1280x720 would add about 46% more pixels; lowering it could
  help GPU-bound scenes, but changing dimensions through the PM4 translator requires handling the game's
  EDRAM-sized render targets. See the correction in `docs/black_ops_switch_performance_research.md`.

### Renderer thread (CPU per draw)

- [ ] Use pc/lr pairs to identify callers behind the known hash/compare/copy cost (~40 % of renderer busy samples:
  `XXH3_hashLong` 7.6 %, `memcmp` 7.5 %, `memmove` 7.4 %). Cut the largest verified caller; geometry-hash cadence
  alone did not produce a clear A/B gain. Vertex uploads are ~1-1.7 MB per frame.
- [ ] If the hottest caller is still unclear, add per-stage renderer timers (PM4 decode, setup, textures, constants,
  uploads, Vulkan calls) and requested-versus-emitted state counters.
- [ ] `bo1_native_record_thread` A/B (exists, off): move NVK's ~4-5 µs per draw recording to a worker. Try only
  after profiling confirms renderer CPU pressure and enough scheduling headroom; verify correctness and frame tails.
- [ ] Reduce texture allocation/copy work: sub-allocate large blocks, upload only mip levels changed by a copy, and
  create images off the ring.
- [ ] Confirm whether the render backend's `WaitForSingleObject` timeout-0 poll/yield loop costs console CPU; it was
  seen in the Mac profile.

### Game threads and system

- [ ] Replace measured game busy-waits with real waits and set thread priorities from the console profile. Job queue
  workers busy in `sub_82545308` / `sub_825450A8` on the Mac; console behavior decides what to change.
- [ ] Range cache for the reread streaming archives (`images.pak`, `snd.all.pak`).
- [ ] `guest_memory_large_pages` A/B (modes 1 and 2).
- [ ] CPU boost mode during loading screens only.
- [ ] Check BO1's guest flip scheduling: Prototype 2's D3D queued flips to later vblanks (35-60 ms of work became
  50/67/83 ms frames). Our slow-frame lines show "game's vblank wait" near 0, so probably not, but confirm.
- [ ] Before testing saves: check the SDK's `XFile::Write`/`SetLength` (returned the size cached at open in
  Prototype) and the `XeKeysConsolePrivateKeySign` / `...SignatureVerification` stubs.

### Faster guest code

- [ ] Direct calls instead of weak aliases.
- [ ] Float moves without FPCR mode switches (`fp_moves_modeless`).
- [ ] Non-volatile registers as locals / no LR stores (needs shared registers for setjmp/SEH; Prototype 2 found its
  exceptions with masseffect-nx's `read_before_write.py` and marks only those functions `share_registers`).
- [ ] "Diet" lowerings.
- [ ] LTO + PGO and hot function ordering; follow the console PGO capture process in Recommended next steps.
- [ ] Per-address locks for lwarx/stwcx. regions instead of one process-wide mutex (Prototype); check how our SDK
  serializes reservations.
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
