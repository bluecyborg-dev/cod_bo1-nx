#include <switch.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HBL_PATH "sdmc:/atmosphere/hbl.nsp"
#define BACKUP_PATH "sdmc:/atmosphere/hbl.nsp.codex-backup"
#define TEMP_PATH "sdmc:/atmosphere/hbl.nsp.codex-temp"

#define PFS0_HEADER_SIZE 0x10u
#define PFS0_ENTRY_SIZE 0x18u
#define NPDM_META_SIZE 0x80u
#define ACID_MIN_SIZE 0x238u
#define ACI0_MIN_SIZE 0x40u

typedef struct {
  size_t acid_descriptor;
  size_t aci0_descriptor;
  uint32_t descriptor;
} CorePatch;

static uint32_t ReadU32(const u8* data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t ReadU64(const u8* data) {
  return (uint64_t)ReadU32(data) | ((uint64_t)ReadU32(data + 4) << 32);
}

static void WriteU32(u8* data, uint32_t value) {
  data[0] = (u8)value;
  data[1] = (u8)(value >> 8);
  data[2] = (u8)(value >> 16);
  data[3] = (u8)(value >> 24);
}

static bool RangeFits(size_t total, size_t offset, size_t length) {
  return offset <= total && length <= total - offset;
}

static bool ReadWholeFile(const char* path, u8** data_out, size_t* size_out) {
  FILE* file = fopen(path, "rb");
  if (!file) {
    printf("Cannot open %s (errno %d).\n", path, errno);
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    printf("Cannot seek %s.\n", path);
    return false;
  }
  const long end = ftell(file);
  if (end <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    printf("Invalid size for %s.\n", path);
    return false;
  }
  u8* data = malloc((size_t)end);
  if (!data) {
    fclose(file);
    printf("Not enough memory to read %s.\n", path);
    return false;
  }
  const size_t got = fread(data, 1, (size_t)end, file);
  const bool ok = got == (size_t)end && ferror(file) == 0;
  fclose(file);
  if (!ok) {
    free(data);
    printf("Short read from %s.\n", path);
    return false;
  }
  *data_out = data;
  *size_out = (size_t)end;
  return true;
}

static bool WriteWholeFileNew(const char* path, const u8* data, size_t size) {
  FILE* file = fopen(path, "wbx");
  if (!file) {
    printf("Cannot create %s (errno %d).\n", path, errno);
    return false;
  }
  const bool ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
  const int close_result = fclose(file);
  if (!ok || close_result != 0) {
    remove(path);
    printf("Write failed for %s.\n", path);
    return false;
  }
  return true;
}

static bool FileExists(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) return false;
  fclose(file);
  return true;
}

static bool FindThreadInfo(u8* npdm, size_t npdm_size, size_t area_offset,
                           size_t area_size, size_t kac_offset_in_area,
                           size_t kac_size_offset, size_t min_area_size,
                           const char* area_name, size_t* descriptor_offset,
                           uint32_t* descriptor_value) {
  if (area_size < min_area_size || !RangeFits(npdm_size, area_offset, area_size)) {
    printf("Invalid %s bounds in main.npdm.\n", area_name);
    return false;
  }
  u8* area = npdm + area_offset;
  const uint32_t kac_offset = ReadU32(area + kac_offset_in_area);
  const uint32_t kac_size = ReadU32(area + kac_size_offset);
  if ((kac_size % sizeof(uint32_t)) != 0 ||
      !RangeFits(area_size, kac_offset, kac_size)) {
    printf("Invalid %s kernel-capability table.\n", area_name);
    return false;
  }

  size_t matches = 0;
  for (size_t offset = kac_offset; offset < (size_t)kac_offset + kac_size;
       offset += sizeof(uint32_t)) {
    const uint32_t descriptor = ReadU32(area + offset);
    // ThreadInfo is the KAC descriptor whose low four bits are 0b0111.
    if ((descriptor & 0xFu) == 0x7u) {
      ++matches;
      *descriptor_offset = area_offset + offset;
      *descriptor_value = descriptor;
    }
  }
  if (matches != 1) {
    printf("Expected one ThreadInfo entry in %s; found %zu.\n", area_name, matches);
    return false;
  }
  return true;
}

static bool LocateCorePermission(u8* npdm, size_t npdm_size, CorePatch* patch) {
  if (npdm_size < NPDM_META_SIZE || memcmp(npdm, "META", 4) != 0) {
    printf("main.npdm has no valid META header.\n");
    return false;
  }

  const uint32_t aci0_offset = ReadU32(npdm + 0x70);
  const uint32_t aci0_size = ReadU32(npdm + 0x74);
  const uint32_t acid_offset = ReadU32(npdm + 0x78);
  const uint32_t acid_size = ReadU32(npdm + 0x7C);
  size_t acid_desc = 0, aci0_desc = 0;
  uint32_t acid_value = 0, aci0_value = 0;

  if (aci0_size < ACI0_MIN_SIZE || !RangeFits(npdm_size, aci0_offset, aci0_size) ||
      memcmp(npdm + aci0_offset, "ACI0", 4) != 0) {
    printf("Invalid ACI0 section in main.npdm.\n");
    return false;
  }
  if (acid_size < ACID_MIN_SIZE || !RangeFits(npdm_size, acid_offset, acid_size) ||
      memcmp(npdm + acid_offset + 0x200, "ACID", 4) != 0) {
    printf("Invalid ACID section in main.npdm.\n");
    return false;
  }

  if (!FindThreadInfo(npdm, npdm_size, acid_offset, acid_size, 0x230, 0x234,
                      ACID_MIN_SIZE, "ACID", &acid_desc, &acid_value) ||
      !FindThreadInfo(npdm, npdm_size, aci0_offset, aci0_size, 0x30, 0x34,
                      ACI0_MIN_SIZE, "ACI0", &aci0_desc, &aci0_value)) {
    return false;
  }
  if (acid_value != aci0_value) {
    printf("ACID and ACI0 ThreadInfo entries differ; refusing to patch.\n");
    return false;
  }

  const uint32_t min_core = (acid_value >> 16) & 0xFFu;
  const uint32_t max_core = (acid_value >> 24) & 0xFFu;
  printf("NPDM core range: %" PRIu32 "-%" PRIu32 " (ACID and ACI0 match).\n",
         min_core, max_core);
  if (min_core > 3 || max_core < min_core) {
    printf("Unexpected ThreadInfo core range; refusing to patch.\n");
    return false;
  }
  patch->acid_descriptor = acid_desc;
  patch->aci0_descriptor = aci0_desc;
  patch->descriptor = acid_value;
  return true;
}

static bool LocateHblNpdm(u8* archive, size_t archive_size, size_t* npdm_offset,
                          size_t* npdm_size) {
  if (archive_size < PFS0_HEADER_SIZE || memcmp(archive, "PFS0", 4) != 0) {
    printf("hbl.nsp is not a PFS0 ExeFS archive.\n");
    return false;
  }
  const uint32_t file_count = ReadU32(archive + 4);
  const uint32_t string_table_size = ReadU32(archive + 8);
  if (file_count == 0 || file_count > 4096 ||
      (size_t)file_count > (SIZE_MAX - PFS0_HEADER_SIZE) / PFS0_ENTRY_SIZE) {
    printf("Invalid PFS0 file table.\n");
    return false;
  }
  const size_t entries_size = (size_t)file_count * PFS0_ENTRY_SIZE;
  const size_t strings_offset = PFS0_HEADER_SIZE + entries_size;
  if (!RangeFits(archive_size, strings_offset, string_table_size)) {
    printf("PFS0 file table or string table exceeds the archive.\n");
    return false;
  }
  const size_t data_offset = strings_offset + string_table_size;
  size_t matches = 0;
  for (uint32_t i = 0; i < file_count; ++i) {
    const u8* entry = archive + PFS0_HEADER_SIZE + (size_t)i * PFS0_ENTRY_SIZE;
    const uint64_t relative_offset = ReadU64(entry);
    const uint64_t file_size = ReadU64(entry + 8);
    const uint32_t name_offset = ReadU32(entry + 0x10);
    if (name_offset >= string_table_size) {
      printf("Invalid PFS0 filename offset.\n");
      return false;
    }
    const char* name = (const char*)(archive + strings_offset + name_offset);
    const size_t remaining = string_table_size - name_offset;
    if (memchr(name, '\0', remaining) == NULL) {
      printf("Unterminated PFS0 filename.\n");
      return false;
    }
    if (relative_offset > SIZE_MAX || file_size > SIZE_MAX ||
        !RangeFits(archive_size - data_offset, (size_t)relative_offset,
                   (size_t)file_size)) {
      printf("PFS0 file entry exceeds the archive.\n");
      return false;
    }
    if (strcmp(name, "main.npdm") == 0) {
      ++matches;
      *npdm_offset = data_offset + (size_t)relative_offset;
      *npdm_size = (size_t)file_size;
    }
  }
  if (matches != 1) {
    printf("Expected one main.npdm in hbl.nsp; found %zu.\n", matches);
    return false;
  }
  return true;
}

static bool RestoreOriginalFromBackup(void) {
  remove(HBL_PATH);
  if (rename(BACKUP_PATH, HBL_PATH) == 0) return true;
  printf("RESTORE FAILED. Rename %s back to %s manually.\n", BACKUP_PATH, HBL_PATH);
  return false;
}

static void PrintHash(const u8 hash[SHA256_HASH_SIZE]) {
  for (size_t i = 0; i < SHA256_HASH_SIZE; ++i) printf("%02x", hash[i]);
  printf("\n");
}

static bool PatchHbl(void) {
  u8* original = NULL;
  size_t size = 0;
  if (!ReadWholeFile(HBL_PATH, &original, &size)) return false;

  u8 original_hash[SHA256_HASH_SIZE];
  sha256CalculateHash(original_hash, original, size);
  printf("HBL SHA-256: ");
  PrintHash(original_hash);

  size_t npdm_offset = 0, npdm_size = 0;
  if (!LocateHblNpdm(original, size, &npdm_offset, &npdm_size)) {
    free(original);
    return false;
  }
  printf("Found main.npdm: %zu bytes.\n", npdm_size);

  CorePatch patch = {0};
  if (!LocateCorePermission(original + npdm_offset, npdm_size, &patch)) {
    free(original);
    return false;
  }
  const uint32_t max_core = (patch.descriptor >> 24) & 0xFFu;
  if (max_core == 3) {
    printf("Core 3 is already allowed. Nothing changed.\n");
    free(original);
    return true;
  }
  if (max_core != 2) {
    printf("Expected current maximum core 2; got %" PRIu32 ". Nothing changed.\n", max_core);
    free(original);
    return false;
  }
  if (FileExists(BACKUP_PATH) || FileExists(TEMP_PATH)) {
    printf("Backup or temporary file already exists. Nothing changed.\n");
    free(original);
    return false;
  }

  u8* patched = malloc(size);
  if (!patched) {
    free(original);
    printf("Not enough memory to prepare patch.\n");
    return false;
  }
  memcpy(patched, original, size);
  const uint32_t patched_descriptor = (patch.descriptor & 0x00FFFFFFu) | (3u << 24);
  WriteU32(patched + npdm_offset + patch.acid_descriptor, patched_descriptor);
  WriteU32(patched + npdm_offset + patch.aci0_descriptor, patched_descriptor);

  u8 patched_hash[SHA256_HASH_SIZE];
  sha256CalculateHash(patched_hash, patched, size);
  printf("Patched HBL SHA-256: ");
  PrintHash(patched_hash);

  if (!WriteWholeFileNew(TEMP_PATH, patched, size)) {
    free(original);
    free(patched);
    return false;
  }
  free(patched);
  u8* temp_data = NULL;
  size_t temp_size = 0;
  if (!ReadWholeFile(TEMP_PATH, &temp_data, &temp_size)) {
    remove(TEMP_PATH);
    free(original);
    return false;
  }
  u8 temp_hash[SHA256_HASH_SIZE];
  sha256CalculateHash(temp_hash, temp_data, temp_size);
  free(temp_data);
  if (temp_size != size || memcmp(temp_hash, patched_hash, SHA256_HASH_SIZE) != 0) {
    remove(TEMP_PATH);
    free(original);
    printf("Temporary file verification failed. Original is untouched.\n");
    return false;
  }

  if (rename(HBL_PATH, BACKUP_PATH) != 0) {
    remove(TEMP_PATH);
    free(original);
    printf("Could not preserve original HBL as backup (errno %d). Nothing changed.\n", errno);
    return false;
  }
  if (rename(TEMP_PATH, HBL_PATH) != 0) {
    const int install_errno = errno;
    RestoreOriginalFromBackup();
    remove(TEMP_PATH);
    free(original);
    printf("Could not install patched HBL (errno %d).\n", install_errno);
    return false;
  }

  u8* installed = NULL;
  size_t installed_size = 0;
  bool installed_ok = ReadWholeFile(HBL_PATH, &installed, &installed_size);
  if (installed_ok) {
    u8 installed_hash[SHA256_HASH_SIZE];
    sha256CalculateHash(installed_hash, installed, installed_size);
    installed_ok = installed_size == size &&
                   memcmp(installed_hash, patched_hash, SHA256_HASH_SIZE) == 0;
  }
  free(installed);
  free(original);
  if (!installed_ok) {
    RestoreOriginalFromBackup();
    printf("Installed HBL failed verification; attempted restore from backup.\n");
    return false;
  }

  printf("PASS: ACID and ACI0 now allow cores 0-3.\n");
  printf("Original backup: %s\n", BACKUP_PATH);
  printf("Exit Sphaira, then reopen HBL for the new process permissions.\n");
  return true;
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  consoleInit(NULL);
  printf("BO1 HBL core 3 patcher\n\n");
  printf("Parses PFS0 and main.npdm before editing either core permission.\n");
  printf("It requires matching ACID/ACI0 entries with max core 2,\n");
  printf("and keeps the original as a backup.\n\n");
  printf("HBL: %s\n", HBL_PATH);
  printf("A: inspect and patch   B: exit\n");

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  bool finished = false;
  while (appletMainLoop() && !finished) {
    padUpdate(&pad);
    const u64 down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_A) {
      consoleClear();
      printf("BO1 HBL core 3 patcher\n\n");
      PatchHbl();
      printf("\nPress B to exit.\n");
      while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_B) break;
        consoleUpdate(NULL);
      }
      finished = true;
    } else if (down & HidNpadButton_B) {
      finished = true;
    }
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  return 0;
}
