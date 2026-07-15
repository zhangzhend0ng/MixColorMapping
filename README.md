# color-mixer-batch

Standalone CLI that runs **both** Snapmaker-Orca color-mixing algorithms on a
table of target colors and reports each algorithm's best recipe side by side.

- **FS** — legacy Justin-Hayes degree-4 polynomial pigment blend
  (`vendor/filament_mixer*`, header-only model).
- **Prusa** — calibrated Yule-Nielsen spectral model
  (`vendor/prusa_fdm_mixer*`), median ΔE2000 ≈ 5.7 vs real FDM prints.

The two algorithm files are vendored **unmodified** from the Snapmaker Orca
slicer — see [`vendor/PROVENANCE.md`](vendor/PROVENANCE.md) for the source
commit and file SHAs.

## What it does

For every target color in your spreadsheet:

1. Normalize the target to 8-bit sRGB (from hex / RGB / Lab / CMYK).
2. Run the **FS** reverse-match search → best `(filamentA, filamentB[, C],
   ratios)` recipe + its predicted color.
3. Run the **Prusa** reverse-match search → best recipe + predicted color.
4. Emit both recipes (hex, RGB, Lab, CMYK + ΔE2000) into one output CSV row.

Both algorithms use the **same ΔE2000 yardstick** (`prusa_fdm_mixer`), so their
ΔE values are directly comparable — this is the same scoring the slicer uses
internally.

## Build

Requires only a C++17 compiler. No cmake needed.

**Windows (MSVC)** — run from a *x64 Native Tools Command Prompt for VS*:
```
build.bat
```

**Linux / macOS / MinGW**:
```
./build.sh
```

Or with cmake:
```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `build/color_match_batch[.exe]`.

## Usage

```
color_match_batch \
  --palette  samples/palette.csv \
  --targets  samples/targets_hex.csv \
  --output   results.csv \
  --input-format hex
```

### Required flags
| Flag | Meaning |
|------|---------|
| `--palette <file>`  | CSV of physical filament colors (one `#hex` per line, optional `,label`) |
| `--targets <file>`  | CSV of target colors (first column = label, rest = color components) |
| `--output <file>`   | Where to write the results CSV |
| `--input-format <fmt>` | `hex` / `rgb` / `lab` / `cmyk` — applies to the whole targets table |

### Optional flags
| Flag | Default | Meaning |
|------|---------|---------|
| `--min-percent <n>`     | `0`   | Min per-component percent (0–50); higher excludes tiny admixtures |
| `--exclude "1-2,3-4"`   | —     | 1-based palette-id pairs to skip (incompatible materials) |
| `--rgb-scale auto\|255\|1` | `auto` | How to read numeric RGB columns |

## Input CSV formats

**palette.csv** — one color per row, `#hex` first (or `label,#hex`):
```
#FF0000,Red
#00FF00,Green
```

**targets.csv** — first column is always a label; the meaning of the remaining
columns depends on `--input-format`:

| Format | Columns (after label) | Example row |
|--------|-----------------------|-------------|
| `hex`  | `#RRGGBB`             | `orange,#FF8800` |
| `rgb`  | `R,G,B`               | `orange,255,136,0` (auto-detects 0-255 vs 0-1) |
| `lab`  | `L,a,b`               | `mid_gray,50,0,0` |
| `cmyk` | `C,M,Y,K`             | `pure_red,0,100,100,0` (percent; auto-handles 0-100 vs 0-1) |

A leading header row (e.g. `label,color`) is auto-detected and skipped.

## Output CSV columns

Each row is one target; columns (in order):

```
label,
target_hex, target_R, target_G, target_B, target_L, target_a, target_b,
target_C, target_M, target_Y, target_K,
fs_type,    fs_ids,    fs_weights,    fs_hex,    fs_R,    fs_G,    fs_B,
fs_L,    fs_a,    fs_b,    fs_C,    fs_M,    fs_Y,    fs_K,    fs_delta_e,
prusa_type, prusa_ids, prusa_weights, prusa_hex, prusa_R, prusa_G, prusa_B,
prusa_L, prusa_a, prusa_b, prusa_C, prusa_M, prusa_Y, prusa_K, prusa_delta_e
```

- `fs_type` / `prusa_type`: `pair` (2 filaments) or `triple` (3 filaments).
- `fs_ids` / `prusa_ids`: `/`-joined 1-based palette indices (e.g. `1/3`).
- `fs_weights` / `prusa_weights`: `/`-joined integer percents summing to 100
  (pair: `pctA/pctB`; triple: `wA/wB/wC`).
- Color columns repeat for both target and each recipe's preview color.

Open `results.csv` directly in Excel, or paste-special it next to your source
sheet.

## Excel workflow

1. Put your filament palette on one sheet → *Save As → CSV* → `palette.csv`.
2. Put your target colors on another sheet → *Save As → CSV* → `targets.csv`.
3. Run the tool (one format at a time).
4. *Data → From Text/CSV* → import `results.csv` onto a new sheet.

## Algorithm notes & caveats

- **FS triple blends are order-sensitive.** The FS polynomial has no native
  N-color form, so triples are folded left-to-right in filament-ID-ascending
  order (matching the slicer's `blend_color_multi`). Results are deterministic
  but would differ with a different fold order — this is inherent to FS.
- **Prusa handles N colors natively** via its Yule-Nielsen weighted average;
  no folding artifact.
- **Lab/CMYK targets lose precision** when normalized to 8-bit sRGB (both
  algorithms operate on uint8 channels). ΔE from a Lab target to its best match
  therefore includes this quantization floor.
- **CMYK uses simple device conversion** (`R = 255·(1-C)·(1-K)`), not ICC
  profile-aware. Fine for relative comparison, not for print-accurate prediction.

## Repository layout

```
color-mixer-batch/
├── CMakeLists.txt        optional cmake build
├── build.bat             one-shot MSVC build
├── build.sh              one-shot g++/clang build
├── README.md             this file
├── src/                  tool sources
│   ├── main.cpp          CLI + CSV I/O
│   ├── match_search.{h,cpp}  reverse-match search (port of slicer logic)
│   └── color_io.{h,cpp}      hex/rgb/lab/cmyk parsing + formatting
├── vendor/               vendored algorithm files (see PROVENANCE.md)
│   ├── filament_mixer.h / .cpp / filament_mixer_model.h
│   ├── prusa_fdm_mixer.hpp / .cpp
│   └── PROVENANCE.md
└── samples/              example CSVs
    ├── palette.csv
    ├── targets_hex.csv
    ├── targets_rgb.csv
    ├── targets_lab.csv
    └── targets_cmyk.csv
```
