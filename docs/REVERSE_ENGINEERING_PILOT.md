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

## Next RE steps

1. If continuing the worker investigation, analyze `sub_825BBB88` and `sub_825DF470` to clarify what condition keeps
   the two callbacks returning “unfinished”; compare their exact control flow with the generated C++.
2. Use this workflow on the next profiled renderer PC/LR hot spot. Query the address and nearby callers, then match
   the result to generated code and the runtime profile.
3. Only propose a native replacement after static control flow and call sites are understood; retain the current Switch
   A/B and rollback process.

No Switch build was produced or sent as part of this pilot.
