# Vendored Color-Mixing Algorithm Sources

These five files implement the two color-mixing models compared by this tool.
They are **unmodified** copies from the Snapmaker Orca slicer. Both models are
self-contained (C++17 standard library only, no external includes).

## Source

- **Repository:** `C:\coil\Projects\SnapmakerOrca_dev` (Snapmaker Orca, feature/mix_filament_phase2)
- **HEAD commit:** `101c9eace15ab3f7d24fda1dacbfc99c2bde4ef9`
- **Copied on:** 2026-07-15
- **Original path prefix:** `src/libslic3r/`

## Files

| File | Algorithm | SHA-256 |
|------|-----------|---------|
| `filament_mixer.h`         | FS — declarations (`Slic3r::filament_mixer_lerp`)   | `c9a29d58...40d8a` |
| `filament_mixer.cpp`       | FS — forwarders + sRGB helpers                      | `327dd4e0...ae461` |
| `filament_mixer_model.h`   | FS — header-only degree-4 polynomial (Justin Hayes) | `13907c2b...58a55` |
| `prusa_fdm_mixer.hpp`      | Prusa — public API (`mix_rgb`, `rgb_to_lab`, …)     | `ff10c37d...bbbca` |
| `prusa_fdm_mixer.cpp`      | Prusa — Yule-Nielsen spectral model                 | `3d22a61e...cee7e` |

## Provenance of the Prusa model itself

The Prusa files are themselves a vendor snapshot inside the slicer, pinned from
upstream `prusa3d/prusa-fdm-mixer` commit `71dc66e` (retrieved 2026-06-22). The
slicer carries two defensive patches on top of upstream (already present in the
copies here — no further local patches were applied):

1. `hex_to_rgb()` parses the leading 6 hex digits and ignores any trailing
   alpha channel (the slicer stores `#RRGGBBAA`; upstream throws on these).
2. `predict_lab()` returns `LAB{0,0,0}` on empty input instead of throwing.

See `src/libslic3r/prusa_fdm_mixer.PROVENANCE.txt` in the slicer repo for the
full upstream file-SHA manifest.

## Why no `ENABLE_PRUSA_FDM_MIXER_DEMO` macro here

That macro lives only in the slicer's *call sites* (it picks which backend to
route through at each `#if/#else`). The algorithm files themselves never
reference it, so this tool can call **both** models simultaneously without any
compile-time switch — the whole point of the A/B comparison.

## Upgrading

To pick up slicer-side changes to either model:

1. Re-copy the five files from the slicer repo into this `vendor/` directory.
2. Regenerate the SHA-256 table above (`sha256sum vendor/*`).
3. Update the HEAD commit and date in this file.
4. Re-run `samples/` regression (see README) to confirm any behavioral shifts.
