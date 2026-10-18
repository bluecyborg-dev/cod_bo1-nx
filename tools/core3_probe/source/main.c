#include <switch.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

static volatile u64 g_work_sink;

static Result ReadTickCount(u64* value, u64 subtype) {
  return svcGetInfo(value, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, subtype);
}

static void RunCoreSample(unsigned core) {
  const u64 mask = 1ULL << core;
  const Result pin_result =
      svcSetThreadCoreMask(CUR_THREAD_HANDLE, (s32)core, mask);
  printf("\nCore %u pin: %s (0x%08X)\n", core,
         R_SUCCEEDED(pin_result) ? "accepted" : "failed",
         (unsigned)pin_result);
  if (R_FAILED(pin_result)) return;

  svcSleepThread(20000000ULL);
  const u32 observed_core = svcGetCurrentProcessorNumber();

  u64 before_total = 0;
  u64 before_core[4] = {0, 0, 0, 0};
  const Result total_before_result =
      ReadTickCount(&before_total, TickCountInfo_Total);
  Result core_before_result[4];
  for (unsigned i = 0; i < 4; ++i) {
    core_before_result[i] = ReadTickCount(&before_core[i], i);
  }

  const u64 start = armGetSystemTick();
  const u64 target = armGetSystemTickFreq() / 5;
  u64 iterations = 0;
  do {
    g_work_sink = (g_work_sink * 1664525ULL) + 1013904223ULL + iterations;
    ++iterations;
  } while (armGetSystemTick() - start < target);
  const u64 elapsed = armGetSystemTick() - start;

  u64 after_total = 0;
  u64 after_core[4] = {0, 0, 0, 0};
  const Result total_after_result =
      ReadTickCount(&after_total, TickCountInfo_Total);
  Result core_after_result[4];
  u64 core_sum = 0;
  bool all_core_queries_succeeded = true;
  for (unsigned i = 0; i < 4; ++i) {
    core_after_result[i] = ReadTickCount(&after_core[i], i);
    if (R_SUCCEEDED(core_before_result[i]) &&
        R_SUCCEEDED(core_after_result[i]) &&
        after_core[i] >= before_core[i]) {
      core_sum += after_core[i] - before_core[i];
    } else {
      all_core_queries_succeeded = false;
    }
  }
  const u32 observed_core_after = svcGetCurrentProcessorNumber();

  printf("Observed CPU before/after sample: %u/%u\n", observed_core,
         observed_core_after);
  printf("Elapsed system ticks: %" PRIu64 " (%.1f ms)\n", elapsed,
         (double)elapsed * 1000.0 / (double)armGetSystemTickFreq());
  printf("Busy-loop iterations: %" PRIu64 "\n", iterations);
  if (R_SUCCEEDED(total_before_result) &&
      R_SUCCEEDED(total_after_result)) {
    printf("Thread total tick delta: %" PRIu64 "\n",
           after_total - before_total);
  } else {
    printf("Thread total query results: before 0x%08X, after 0x%08X\n",
           (unsigned)total_before_result, (unsigned)total_after_result);
  }
  for (unsigned i = 0; i < 4; ++i) {
    if (R_SUCCEEDED(core_before_result[i]) &&
        R_SUCCEEDED(core_after_result[i])) {
      printf("  core %u tick delta: %" PRIu64 "\n", i,
             after_core[i] - before_core[i]);
    } else {
      printf("  core %u query results: before 0x%08X, after 0x%08X\n", i,
             (unsigned)core_before_result[i],
             (unsigned)core_after_result[i]);
    }
  }
  if (R_SUCCEEDED(total_before_result) &&
      R_SUCCEEDED(total_after_result) && all_core_queries_succeeded &&
      after_total >= before_total) {
    const u64 total_delta = after_total - before_total;
    const u64 difference = core_sum > total_delta ? core_sum - total_delta
                                                   : total_delta - core_sum;
    const u64 tolerance = total_delta / 100 > 2048 ? total_delta / 100 : 2048;
    printf("Per-core sum: %" PRIu64 " vs total: %" PRIu64
           "; accounting: %s\n",
           core_sum, total_delta,
           difference <= tolerance ? "PASS" : "FAIL");
  } else {
    printf("Per-core accounting: unavailable (query failed or counter regressed)\n");
  }
}

static void RunProbe(void) {
  u64 process_mask = 0;
  const Result info_result = svcGetInfo(&process_mask, InfoType_CoreMask,
                                        CUR_PROCESS_HANDLE, 0);
  if (R_SUCCEEDED(info_result)) {
    printf("Process core mask: 0x%llX\n", (unsigned long long)process_mask);
  } else {
    printf("Process core mask query failed: 0x%08X\n",
           (unsigned)info_result);
  }

  s32 original_preferred = -1;
  u64 original_mask = 0;
  const Result get_mask_result = svcGetThreadCoreMask(
      &original_preferred, &original_mask, CUR_THREAD_HANDLE);
  if (R_FAILED(get_mask_result)) {
    printf("Could not read thread affinity: 0x%08X\n",
           (unsigned)get_mask_result);
    return;
  }
  printf("Initial thread affinity: preferred %d, mask 0x%llX\n",
         original_preferred, (unsigned long long)original_mask);
  printf("TickCountInfo uses InfoType_ThreadTickCount (HOS 13+).\n");

  for (unsigned core = 0; core < 4; ++core) {
    RunCoreSample(core);
  }

  const Result restore_result = svcSetThreadCoreMask(
      CUR_THREAD_HANDLE, original_preferred, (u32)original_mask);
  printf("\nOriginal affinity restore: %s (0x%08X)\n",
         R_SUCCEEDED(restore_result) ? "PASS" : "failed",
         (unsigned)restore_result);
  printf("Work sink: %" PRIu64 "\n", g_work_sink);
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  consoleInit(NULL);
  printf("BO1 per-core tick counter check\n");
  printf("Pins this probe thread to each core for a short busy-loop sample.\n");
  printf("It restores its original affinity when finished.\n");
  RunProbe();
  printf("\nPress B to exit.\n");

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    consoleUpdate(NULL);
    if (padGetButtonsDown(&pad) & HidNpadButton_B) break;
  }
  consoleExit(NULL);
  return 0;
}
