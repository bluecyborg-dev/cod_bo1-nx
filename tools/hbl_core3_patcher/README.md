# HBL core 3 patcher

This Switch NRO raises the maximum permitted CPU core for the process launched by
`sdmc:/atmosphere/hbl.nsp`, from core 2 to core 3. It preserves the original
file as `sdmc:/atmosphere/hbl.nsp.codex-backup` and does not commit or package
the user's HBL NSP.

The patcher checks the PFS0 table, finds exactly one `main.npdm`, validates its
META, ACID, and ACI0 regions, and locates the ThreadInfo kernel-capability entry
in both permission tables. It only writes when both descriptors are identical,
their current maximum core is 2, and their minimum core is at most 3. Any unknown
layout or inconsistent permission stops without modifying HBL. A temporary
replacement is read back and hash-checked before the original is renamed to the
backup and the patched file is installed.

## Build

With devkitPro installed:

```sh
make
```

The resulting `hbl_core3_patcher.nro` can be sent using the existing Sphaira
NetLoader + `nxlink` workflow. Launch it from the SD card, press **A** to
inspect and patch, and follow the result printed on screen. After a successful
patch, exit Sphaira and reopen HBL so the next process starts with the updated
permissions. Keep the backup on the SD card for rollback.

The patch grants the process permission to use core 3. It does not reserve that
core from Horizon system services. Verify the resulting process core mask and
thread CPU usage in `logs/rex/rex_profile.log` before comparing performance.
