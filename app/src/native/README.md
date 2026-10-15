# Native renderer (GPL-3.0-or-later)

Copied from project8-sw (R-drg, `src/game/src/native/`), which adapted it from nfsmw-nx (StevensND,
`app/src/nfsmw_nativo_sistema.*`). Both are GPL-3.0; so is this folder, and any build that links it.

Changes for BO1: names (`thps_*` settings → `bo1_*`, namespace `thps` → `bo1`), the pacing counters header
(`../bo1_pacing_stats.h`). Everything else is as upstream; see project8-sw's `docs/native-renderer.md` for the design.
Selected with `bo1_renderer = "native"`; the default stays the SDK's emulated GPU (`xenos`).
