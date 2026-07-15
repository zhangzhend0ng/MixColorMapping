// match_search.h — generic reverse color-match search.
//
// The search loop is a 1:1 port of the slicer's
// `MixedColorMatchHelpers::build_best_color_match_recipe` (see PROVENANCE for
// the source commit). The only generalization: the blend step is injected as a
// pair of function pointers (`BlendFns`), so the FS polynomial backend and the
// Prusa Yule-Nielsen backend can each run the same search independently,
// producing two recipes to compare side by side.
//
// Both backends are scored with the SAME ΔE2000 yardstick (prusa_fdm_mixer),
// matching the slicer's own A/B test — so the ΔE values are directly
// comparable across the two algorithms.
#pragma once

#include "color_io.h"

#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cmb {

// A pair of blend functions — one for 2 colors, one for N (≥3) colors.
// Both return the predicted CIELab. Implementations live in match_search.cpp.
struct BlendFns {
    // Blend two colors with t_b = fraction of B in [0,1] (0 ⇒ pure A, 1 ⇒ pure B).
    std::function<Lab(RGB8 a, RGB8 b, double t_b)> pair_lab;

    // Blend N colors (ids sorted ascending by the caller) with weights summing
    // to 1. Used for the triple search. Order-sensitive for the FS backend.
    std::function<Lab(const std::vector<RGB8>& colors, const std::vector<double>& weights)> multi_lab;

    // Human-readable name for logging / output columns.
    std::string name;
};

// Result of matching one target color against a palette.
struct MatchResult {
    enum class Type { Pair, Triple };
    Type        type = Type::Pair;
    bool        valid = false;

    // 1-based palette indices, sorted ascending.
    std::vector<unsigned> ids;

    // Integer percents (sum to 100). For Pair: [pct_a, pct_b].
    // For Triple: [wA, wB, wC].
    std::vector<int> weights;

    RGB8   preview_rgb;
    Lab    preview_lab;
    double delta_e = std::numeric_limits<double>::infinity();
};

// Two ready-made BlendFns instances (defined in match_search.cpp):
BlendFns fs_blend();      // legacy Justin-Hayes degree-4 polynomial
BlendFns prusa_blend();   // calibrated Yule-Nielsen spectral model

// Run the full reverse-match search for one target.
//
// palette           physical colors to search over (size ≥ 2 for pair search,
//                   ≥ 3 for triple search).
// target_lab        target color in CIELab.
// blend             which algorithm's blend functions to use.
// min_percent       minimum per-component percent (0-50). Mirrors the slicer's
//                   `min_component_percent`.
// exclude_pairs     1-based filament-id pairs to skip (mutually incompatible
//                   materials). Empty = all pairs allowed.
MatchResult search_best(const std::vector<RGB8>& palette,
                        Lab target_lab,
                        const BlendFns& blend,
                        int min_percent,
                        const std::vector<std::pair<int, int>>& exclude_pairs);

// ---------------------------------------------------------------------------
// CachedSearcher — amortizes the pair-LUT build across many target colors.
//
// The pair BlendLUT depends only on (palette, blend), not on the target. For a
// batch of N targets with the same palette, the naive search_best rebuilds the
// LUT N times. CachedSearcher builds it once and reuses it, turning an O(N×P²)
// cost into O(P² + N×search). This is the difference between 1.3s and ~50ms
// for 30 targets on an 8-color palette.
// ---------------------------------------------------------------------------
class CachedSearcher {
public:
    CachedSearcher(const BlendFns& blend, const std::vector<RGB8>& palette);

    // Re-match one target against the cached palette/LUT.
    MatchResult match_one(Lab target_lab,
                          int min_percent,
                          const std::vector<std::pair<int, int>>& exclude_pairs) const;

    const std::vector<RGB8>& palette() const { return m_palette; }
private:
    BlendFns             m_blend;
    std::vector<RGB8>    m_palette;
    std::vector<Lab>     m_palette_lab;
    // pair LUT: [a][b-a][percent], built once per palette.
    std::vector<std::vector<std::vector<Lab>>> m_pair_lut;

    // Triple coarse-scan cache. Key: packed (a,b,c) 0-based indices with
    // a<b<c. Value: all coarse-grid (step=10) weight Labs for that triple,
    // precomputed once so every target reuses them instead of re-running the
    // expensive multi_lab. Each entry: (wa, wb, wc, Lab).
    struct TripleKey {
        unsigned a, b, c; // 0-based, sorted
        bool operator==(const TripleKey& o) const { return a==o.a && b==o.b && c==o.c; }
    };
    struct TripleKeyHash {
        size_t operator()(const TripleKey& k) const {
            return (size_t(k.a) << 40) ^ (size_t(k.b) << 20) ^ size_t(k.c);
        }
    };
    struct TripleEntry { int wa, wb, wc; Lab lab; };
    std::unordered_map<TripleKey, std::vector<TripleEntry>, TripleKeyHash> m_triple_cache;

    void build_lut();
    void build_triple_cache(int loop_min);
};

} // namespace cmb
