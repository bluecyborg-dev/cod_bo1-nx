# REA/Ghidra reverse-engineering pilot

**Date:** 2026-10-18
**Branch:** `codex-reverse-engineering`
**Scope:** read-only static analysis of the local Xbox 360 `default.xex`; no game bytes or Switch build were changed.

## Assessment

Targeted reverse engineering is feasible and useful here. REA can query selected functions, references, callers, callees,
and strings through Ghidra. The pilot recovered useful structure around the job-queue worker when its decoded listing was
compared with the project's XenonRecomp output and existing runtime profile.

This does **not** recover the original source code or produce a safe replacement executable. The port already recompiles
the XEX to C++ with XenonRecomp (`tools/codegen.sh` → `app/generated/`); changes to game behavior still need to go through
that code, a guarded native replacement, or a runtime hook. REA is an investigation aid for finding and understanding
targets, not a substitute for XenonRecomp or the Switch build.

## Input and analysis carrier

The repository's `game/default.xex` was an Xbox 360 XEX2 image (PE machine `0x01F2`), SHA-256:

```
6027bd4fa4edc750fc60f544ef51feca766290f8a2785e91ebdda6368f281851
```

REA's documented Ghidra PPC lane accepts ELF32 big-endian generic PowerPC, not this XEX directly. The raw-binary lane
does not list PPC support. The XEX was extracted to a loaded PE image with the existing local reference extractor,
then `tools/xex_image_to_analysis_elf.py` wrapped that image in a **synthetic analysis carrier**. The wrapper preserves
the loaded bytes and addresses; it does not decrypt XEX files, alter the image, or make a runnable ELF.

The carrier remains ELF32 so its addresses and loader ranges fit the image. Xenon guest code also uses 64-bit GPR
instructions (`ld`/`std`), which generic 32-bit PPC cannot decode. Setting REA's language override to
`PowerPC:BE:64:64-32addr` gave Ghidra a 64-bit instruction model with 32-bit addresses. REA accepted that override for
this target; this is a locally validated configuration beyond the documented generic ELF32 PPC lane, not a claimed
official Xenon profile.

Observed image layout:

- image base: `0x82000000`
- loaded image size: `0x02070000`
- entry point: `0x824DBDB8`
- carrier: ELF32 big-endian, `EM_PPC`, generic `e_flags=0`
- carrier SHA-256: `69291ee66020a424d9e60bc8b5abc4e3a340f9b43df3c7caa3104ff1d6a103cc`

Rebuilding the carrier with the committed wrapper produced the same SHA-256 as the original pilot carrier. The script
expects an already extracted image and refuses non-Xbox-360 PE machine values, invalid section ranges, or an entry point
outside executable sections.

## Tools and repeatable query

This Mac pilot used Node `24.11.0`, REA `6.5.0`, Ghidra `12.1.4`, and Temurin JDK `25.0.4+1`; the Ghidra native
decompiler was built for the host architecture. These were installed under `/tmp/bo1-rea-poc`, not registered globally.
The REA doctor check reported the Ghidra provider healthy. The query import was ephemeral and did not write to the XEX
or carrier.

The local XEX extraction step used the existing ignored reference checkout at
`refs/bo1-recomp/tools/xex_extract.py` (with its PyCryptodome environment). On a machine with that checkout and the
analysis tools installed, the carrier/query portion is:

```sh
python3 refs/bo1-recomp/tools/xex_extract.py game/default.xex /tmp/bo1-game-image.bin
python3 tools/xex_image_to_analysis_elf.py /tmp/bo1-game-image.bin /tmp/bo1-analysis.elf
cat >/tmp/bo1-seeds.tsv <<'EOF'
0x825450a8 function sub_825450A8
0x82545308 function sub_82545308
0x825444b8 function sub_825444B8
0x82546470 function sub_82546470
0x825bbfd0 function sub_825BBFD0
0x825bc040 function sub_825BC040
EOF

GHIDRA_INSTALL_DIR=/path/to/ghidra \
JAVA_HOME=/path/to/jdk \
GHIDRA_HEADLESS_MAXMEM=4G \
REA_GHIDRA_STARTUP_TIMEOUT_MS=600000 \
REA_GHIDRA_LANGUAGE_ID=PowerPC:BE:64:64-32addr \
REA_GHIDRA_DISABLED_ANALYZERS="PowerPC Constant Reference Analyzer" \
REA_GHIDRA_SEED_FILE=/tmp/bo1-seeds.tsv \
rea function /tmp/bo1-analysis.elf 0x82545308 --provider ghidra --json
```

The six-function seed file used in this pilot names known worker/job-queue entry points from the generated code. Seeds
are analyst-supplied hints, not discoveries made by Ghidra. With the generic 32-bit language, a 2 GB Ghidra heap ran
out of memory in `PowerPCAddressAnalyzer.flowConstants`; the successful 64-bit-address-mode worker query used 4 GB
and disabled `PowerPC Constant Reference Analyzer`. These are machine-specific settings to record and revisit, not a
claim that all REA PPC analysis needs them.

The wrapper and pilot use the analysis lane described by REA's [native investigation guide](https://github.com/morluto/rea/blob/main/docs/native-investigation.md),
[Ghidra PPC guide](https://github.com/morluto/rea/blob/main/docs/ghidra-ppc.md), and
[raw binary limitations](https://github.com/morluto/rea/blob/main/docs/raw-binary.md).

## Findings

REA examined these seeded functions in the worker path:

- `0x82545308` (`sub_82545308`): under `PowerPC:BE:64:64-32addr`, Ghidra reported the complete function body
  (`0x82545308`–`0x82545613`, 780 bytes). The worker calls `sub_825450A8` at `0x825453A0` to obtain a job. It invokes
  the job callback indirectly via `bctrl` at `0x825453F4`; a nonzero callback result reaches `sub_82546470` at
  `0x82545528` to requeue the job. With no job, the loop calls `sub_825444B8` at `0x825455F8` before returning to the
  lookup. The decoded listing and pseudocode match the generated C++ in `app/generated/default/bo1_recomp.83.cpp`.
- `0x825BBFD0` (`sub_825BBFD0`): Ghidra reported a complete 108-byte function. Its callback returns `1` while
  `sub_825BBB88()` is nonzero. Otherwise it checks `sub_825DF470()`; when that returns zero, it calls
  `sub_8238F608()` and returns zero. This matches the generated callback in `app/generated/default/bo1_recomp.18.cpp`
  and confirms why the worker can keep requeuing these jobs while the shared condition remains active.
- `0x825450A8` (`sub_825450A8`): the generated code scans the priority lists to find work. This fits the existing
  profile's worker samples and the spin explanation documented in `app/src/bo1_engine.cpp`. Its first REA query used
  the wrong 32-bit language and truncated at the save-register helper, so no new claim about its Ghidra decompilation
  is made here.
- `0x82546470` (`sub_82546470`): this is the job-list requeue routine used at the worker's unfinished-job path. Its
  first REA query also used the wrong 32-bit language and truncated at the save-register helper; its role is
  corroborated by the complete caller listing, XenonRecomp output, and existing hook.

The worker's unfinished-job loop is already throttled by `bo1_job_worker_sleep_us`; the pilot found no new optimization
to apply. It does give us a way to inspect a specific function from a sampled PC/LR and compare the result with the
generated C++ before proposing another change.

## Follow-up: dvar guard and worker condition (2026-10-18)

The same synthetic ELF carrier was reanalyzed with Ghidra 12.1.4 for the PC-decompilation leads. The complete
`sub_823949E0` body (288 bytes) reads and writes the dvar flags as 16-bit values at offset `+0x0C`. The generated
`sub_82393370` listing checks bit `0x80`, loads a pointer from `0x832EF528`, and reads the byte at pointer `+0x18`;
when that byte is false on the relevant source path, it can emit the XEX string `%s is cheat protected.` at
`0x82057634`. The image also contains the string `sv_cheats` at `0x8202F250`, but the current reference inventory does
not connect that string to the global pointer, so its identity remains unconfirmed. This establishes the flag layout;
the completed runtime audit below records the live dvar values and preset transition.

The XEX has strings for `r_lodScaleSkinned`, `r_lodBiasSkinned`, `r_enableDlights`, and `sm_sunSampleSizeNear`, but no
`r_fastSkin` string. Presence of a name does not establish that the variable is registered in the target game mode,
what its flags/defaults are, or whether changing it improves performance.

The REA query for `sub_825BBB88` exposed only an 8-byte thunk that calls `0x825F8434`; it did not recover the larger
predicate body used by the generated project. XenonRecomp's generated implementation reads global address
`0x83FB7220`, then checks additional per-job state before returning unfinished. A direct release store to
`0x83FB7220` was not found in the generated-code search. That is not proof there is no writer: indirect stores or code
outside the searched path remain possible. The PC claim that this state is the prior-frame dynamic-buffer upload
condition, and the proposed event wait, therefore remain unverified on 360. Keep the measured timed-requeue throttle
until the matching producer is identified.

The diagnostic Switch build `out/elf-archive/bo1-nx-20261018-dvar-audit.nro` adds a one-pass report of candidate and
shadow dvar flags/current/latched values after the initial command batch and verifies `sm_sunShadowScale` direct writes
by reading both fields back. It does not change which dvars are written. The build passed with `_start` at address zero
and was sent with `--bo1_shadows=min --bo1_profile_log=true`, without `devmap`; the complete capture is
`out/net_logs/20261018-203322.log`. The user confirmed the game was already in Kino. The initial audit found
`sm_qualitySpotShadow`, `r_spotLightEntityShadows`, and `r_spotLightSModelShadows` at flag `0x80` and current/latched
zero; `sm_sunShadowScale` and `sm_sunSampleSizeNear` were both 0.25, `sm_maxLights=1`, and `sm_enable=1`. The skinned
LOD dvars were present with flag `0x0001`, values 0 and 1. `r_enableDlights` was absent from the runtime dvar pool,
although its name exists in the XEX; `r_fastSkin` has no XEX string. No cheat-protection message appears anywhere in
the full capture. Because the audit ran only after the initial setting batch, it cannot prove that `set` changed the
cheat-flagged variables; a pre-batch value or a controlled direct readback is still needed.

At 20:39:36.668, the user switched the live shadow preset from `min` to `off`. The log records `set sm_enable 0`; the
port's direct write of `sm_sunShadowScale=1.0` passed current/latched readback, and the follow-up audit reported
`sm_enable=0`, `sm_sunShadowScale=1.0`, and `sm_sunSampleSizeNear=0.93`. Across 25 steady 10-second `min` reports,
FPS averaged 16.9 (median 17.4, range 9.8–20.0), renderer CPU 41.1 ms/frame, and GPU 34.5 ms/frame. The 11 steady
reports after switching to `off` averaged 18.57 FPS (median 19.0, range 15.2–19.9), renderer CPU 31.8 ms/frame, and
GPU 30.8 ms/frame. That is a positive signal, but the run had one sequential transition while the active scene varied,
so it does not establish an isolated shadow-off performance gain. The full 37-window gameplay capture averaged
17.37 FPS (median 17.6, range 9.8–20.0). Job-queue reports averaged 323,817 unfinished requeues and 176,637 100 µs
sleeps per 10 seconds. The high counts keep the 360 worker-condition investigation relevant but do not identify the
producer or validate the PC dynamic-buffer explanation. The log has no fatal/crash marker and the stream closed after
the game exited.

## Reliability limits

Treat this pilot as static evidence with some unresolved limits:

- The initial `PowerPC:BE:32:default` queries reported bad instructions at 64-bit `ld`/`std` operations and treated
  Xenon save-register helpers as non-returning, truncating functions. Re-running the worker and callback with
  `PowerPC:BE:64:64-32addr` decoded their full function bodies and resolved the list requeue callee.
- The 64-bit-mode decompiler models reservation/conditional-store code (`lwarx`/`stwcx.`) awkwardly, introducing
  synthetic `in_RESERVE` state and loops. Use the disassembly and generated PPC C++ as the authority for those details.
- Auto-analysis also logged p-code errors in other functions elsewhere in the full image. The selected worker still had
  complete address coverage, but broader binary analysis needs its own validation.
- Names and initial function boundaries were seeded from the known generated-code addresses. Ghidra did not discover
  those names independently.
- `PowerPC:BE:64:64-32addr` is an explicit language override on a synthetic ELF32 carrier, not proof that Ghidra's
  generic 64-bit PPC model covers every Xbox 360 Xenon instruction or calling convention.
- Static decompilation does not measure Switch scheduling, performance, visuals, or runtime correctness. Any change
  prompted by this work still needs the existing build and console validation workflow.

REA's [native investigation guide](https://github.com/morluto/rea/blob/main/docs/native-investigation.md) describes
static analysis and evidence capture. For actual Xenon recompilation, retain the existing
[XenonRecomp code-generation pipeline](https://github.com/rexglue/rexglue-sdk/wiki/Codegen-Pipeline-Overview); REA does
not replace that pipeline.

## Switch profiler capture ready for REA

The existing archived build is sufficient; no additional Switch NRO is needed to capture addresses for this workflow:
`out/elf-archive/bo1-nx-20261018-hitch-stacks-oc-core-ab.nro`, with its matching unstripped ELF
`out/elf-archive/bo1-20261018-hitch-stacks-oc-core-ab.elf` and symbol listing. It includes the long-frame marker and
stack profiler. Run it with stack sampling and log forwarding enabled:

```sh
NRO=out/elf-archive/bo1-nx-20261018-hitch-stacks-oc-core-ab.nro \
EXTRA_ARGS="--bo1_debug_exec=devmap+zombie_theater --bo1_debug_exec_delay_s=150 --bo1_profile_log=true --bo1_profile_stacks=true" \
tools/nxlink_run.sh
```

The map loads automatically. Reports include per-thread samples and PC/LR pairs during frames of at least 100 ms. These
are host addresses in the recompiled NRO: resolve them with the matching ELF first, then use the resulting generated
`sub_...` function/address as the REA query seed. Stack sampling is a diagnostic mode that can perturb pacing; use it
to locate code paths, not to compare frame rate. The auto-loaded map reaches the spawn point; an active-play capture can
cover combat-specific paths and naturally occurring stalls. The launch, forwarding, and symbol
resolution workflow is documented in `docs/BUILDING_NRO.md` and `docs/SWITCH_OPTIMIZATION_GUIDE.md`.

### Results from the Switch capture

The 2026-10-18 capture is `out/net_logs/20261018-182410.log`; it used the archived NRO and matching ELF listed above.
The map loaded automatically and produced 68 post-load ten-second reports. Stack PCs were resolved against that ELF:
the renderer samples repeatedly reached `memcmp`, `Renderer::Impl::RangeHash`, `FlatMap64::operator[]`, and
`TextureCache::GetVolumeView`; audio samples reached `XmaContext::Work`, `decode_frame`, and
`ff_wma_run_level_decode`. The REA worker targets also appeared in the capture: `image+0xe3f4f0` resolves to
`sub_825BBB88`, sampled on Server, Backend, and JQ Worker threads, while the `image+0x872090` region resolves to
`sub_825DF470` in Server/JQ Worker PC/LR pairs. Their individual sample shares were small, so treat them as leads to
inspect in REA and generated code, not as proof of dominant CPU cost. The paired addresses provide further REA seeds:
samples at `sub_825BBB88` carried LR-side symbols `sub_823BBCB8`, `sub_825DE5D0`, `sub_823AFDB8`,
`sub_825C9B50`, and `sub_82546470`; samples with LR at `sub_825DF470` had PC-side symbols including
`sub_8222C298`, `sub_825D1638`, and `sub_8236ED48`. These are sampled execution contexts; inspect the corresponding
Ghidra call sites and compare them with XenonRecomp output before assigning exact call semantics.

The profiler reported workload/process masks `0xF` and process CPU above 300% in 11 reports (355% peak), showing
that process threads ran on core 3 at least some of the time. However, the effective clocks stayed at stock handheld
values (CPU 1,020, GPU 460.8, memory 1,331.2 MHz), so this was not an overclock capture or a core-mask A/B. The
native renderer recorded 45 post-load frames over 100 ms; the longest was 4.331 s, including 4.311 s of ring idle.
That shows the renderer received no draw work during the pause, rather than waiting on a multi-second GPU operation.
Because 1 ms stack sampling can perturb pacing, those frame-rate and hitch counts are diagnostic only.

## Next RE steps

1. If continuing the worker investigation, analyze `sub_825BBB88` and `sub_825DF470` to clarify what condition keeps
   the two callbacks returning “unfinished”; compare their exact control flow with the generated C++.
2. Use this workflow on the next profiled renderer PC/LR hot spot. Query the address and nearby callers, then match
   the result to generated code and the runtime profile.
3. Only propose a native replacement after static control flow and call sites are understood; retain the current Switch
   A/B and rollback process.

The static analysis pilot itself did not produce a Switch build. A later focused direct-call ABI candidate and its
Switch FPS run are recorded in `docs/OPTIMIZATION_LOG.md`.
