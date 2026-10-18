#include <switch.h>

#include <stdbool.h>
#include <stdio.h>

static void RunProbe(void) {
  u64 process_mask = 0;
  const Result info_result = svcGetInfo(&process_mask, InfoType_CoreMask,
                                        CUR_PROCESS_HANDLE, 0);
  printf("Process core mask: ");
  if (R_SUCCEEDED(info_result)) {
    printf("0x%llX\n", (unsigned long long)process_mask);
  } else {
    printf("unavailable (0x%08X)\n", (unsigned)info_result);
  }

  s32 original_preferred = -1;
  u64 original_mask = 0;
  const Result get_mask_result = svcGetThreadCoreMask(
      &original_preferred, &original_mask, CUR_THREAD_HANDLE);
  if (R_FAILED(get_mask_result)) {
    printf("Could not read current thread affinity: 0x%08X\n",
           (unsigned)get_mask_result);
    return;
  }
  printf("Initial thread affinity: preferred %d, mask 0x%llX\n",
         original_preferred, (unsigned long long)original_mask);

  const Result set_result = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, 0x8);
  printf("Pin to core 3: %s (0x%08X)\n",
         R_SUCCEEDED(set_result) ? "accepted" : "rejected",
         (unsigned)set_result);
  if (R_FAILED(set_result)) return;

  bool ran_on_core3 = false;
  u32 observed_core = 0xFFFFFFFFu;
  for (unsigned i = 0; i < 100; ++i) {
    observed_core = svcGetCurrentProcessorNumber();
    if (observed_core == 3) {
      ran_on_core3 = true;
      break;
    }
    svcSleepThread(1000000ULL);
  }
  printf("Observed CPU: %u\n", observed_core);
  printf("Core 3 execution: %s\n", ran_on_core3 ? "PASS" : "not observed");

  const Result restore_result = svcSetThreadCoreMask(
      CUR_THREAD_HANDLE, original_preferred, (u32)original_mask);
  printf("Original affinity restore: %s (0x%08X)\n",
         R_SUCCEEDED(restore_result) ? "PASS" : "failed",
         (unsigned)restore_result);
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  consoleInit(NULL);
  printf("BO1 core 3 permission probe\n\n");
  printf("This reads the process mask, pins this probe thread to core 3,\n");
  printf("checks which CPU it runs on, then restores its original affinity.\n\n");
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
