// match_search.cpp — see match_search.h.
//
// The search loop mirrors MixedColorMatchHelpers.cpp:421-650 in the slicer
// (commit 101c9eace). The slicer version is welded to a single backend at
// compile time via ENABLE_PRUSA_FDM_MIXER_DEMO and to wxColour; here the blend
// math is injected so both backends run the identical search and can be
// compared on the same target.
#include "match_search.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <queue>
#include <vector>

#include "filament_mixer.h"     // vendored FS backend
#include "prusa_fdm_mixer.hpp"  // vendored Prusa backend

namespace cmb {

// ===========================================================================
// BlendFns implementations
// ===========================================================================

BlendFns fs_blend()
{
    BlendFns f;
    f.name = "fs";

    f.pair_lab = [](RGB8 a, RGB8 b, double t_b) -> Lab {
        unsigned char r = 0, g = 0, bb = 0;
        Slic3r::filament_mixer_lerp(a.r, a.g, a.b, b.r, b.g, b.b,
                                    static_cast<float>(t_b), &r, &g, &bb);
        return rgb_to_lab(RGB8{ r, g, bb });
    };

    // FS has no native N-color blend; the slicer's blend_color_multi / multi
    // filament mixer folds pairs left-to-right in filament-id-ascending order
    // with t = next_pct / accumulated. We replicate that exactly so the FS
    // triple results match what the slicer would produce.
    f.multi_lab = [](const std::vector<RGB8>& colors, const std::vector<double>& weights) -> Lab {
        if (colors.empty() || colors.size() != weights.size())
            return Lab{ 50.0, 0.0, 0.0 };

        // Accumulate as integer percents to match the slicer's path; doubles
        // would drift. Use a 10000 scale for 2-digit precision.
        auto to_pct100 = [](double w) { return static_cast<int>(std::lround(w * 100.0)); };

        unsigned char r = colors[0].r, g = colors[0].g, b = colors[0].b;
        int accumulated = to_pct100(weights[0]);
        for (size_t i = 1; i < colors.size(); ++i) {
            const int next_pct = to_pct100(weights[i]);
            const int total = accumulated + next_pct;
            if (total <= 0) continue;
            const float t = static_cast<float>(next_pct) / static_cast<float>(total);
            unsigned char nr = 0, ng = 0, nb = 0;
            Slic3r::filament_mixer_lerp(r, g, b, colors[i].r, colors[i].g, colors[i].b, t, &nr, &ng, &nb);
            r = nr; g = ng; b = nb;
            accumulated = total;
        }
        return rgb_to_lab(RGB8{ r, g, b });
    };

    return f;
}

BlendFns prusa_blend()
{
    BlendFns f;
    f.name = "prusa";

    f.pair_lab = [](RGB8 a, RGB8 b, double t_b) -> Lab {
        const double ta = 1.0 - t_b;
        std::vector<prusa_fdm_mixer::Part> parts = {
            { to_hex(a), ta },
            { to_hex(b), t_b },
        };
        const prusa_fdm_mixer::RGB rgb = prusa_fdm_mixer::mix_rgb(parts);
        return rgb_to_lab(RGB8{ rgb.r, rgb.g, rgb.b });
    };

    f.multi_lab = [](const std::vector<RGB8>& colors, const std::vector<double>& weights) -> Lab {
        if (colors.empty() || colors.size() != weights.size())
            return Lab{ 50.0, 0.0, 0.0 };
        std::vector<prusa_fdm_mixer::Part> parts;
        parts.reserve(colors.size());
        for (size_t i = 0; i < colors.size(); ++i) {
            const double w = weights[i] < 0.0 ? 0.0 : weights[i];
            if (w > 0.0) parts.push_back({ to_hex(colors[i]), w });
        }
        const prusa_fdm_mixer::RGB rgb = prusa_fdm_mixer::mix_rgb(parts);
        return rgb_to_lab(RGB8{ rgb.r, rgb.g, rgb.b });
    };

    return f;
}

// ===========================================================================
// search_best — port of build_best_color_match_recipe
// ===========================================================================

namespace {

// Small LUT so the pair search reuses blended Labs instead of recomputing.
// Indexed [a][b-a][percent], b >= a, percent 0..100.
class PairLUT {
public:
    explicit PairLUT(size_t n, const BlendFns& blend, const std::vector<RGB8>& palette)
        : m_n(n)
    {
        m_data.assign(n, {});
        for (size_t a = 0; a < n; ++a) {
            m_data[a].assign(n - a, {});
            for (size_t b = a; b < n; ++b) {
                m_data[a][b - a].resize(101);
                for (int pct = 0; pct <= 100; ++pct) {
                    m_data[a][b - a][pct] = blend.pair_lab(palette[a], palette[b], pct / 100.0);
                }
            }
        }
    }
    const Lab& get(size_t a, size_t b, int pct) const {
        assert(a <= b && pct >= 0 && pct <= 100);
        return m_data[a][b - a][pct];
    }
private:
    size_t m_n;
    std::vector<std::vector<std::vector<Lab>>> m_data;
};

inline bool excluded(size_t a1, size_t b1,
                     const std::vector<std::pair<int, int>>& exclude_pairs)
{
    // exclude_pairs are 1-based; normalize.
    const int ai = static_cast<int>(a1) + 1;
    const int bi = static_cast<int>(b1) + 1;
    for (const auto& pr : exclude_pairs) {
        if ((pr.first == ai && pr.second == bi) || (pr.first == bi && pr.second == ai))
            return true;
    }
    return false;
}

// Build the RGB8 preview of a pair recipe (pct_b percent of B).
RGB8 preview_pair(const std::vector<RGB8>& palette, const BlendFns& blend,
                  size_t a, size_t b, int pct_b)
{
    // round-trip through Lab is what the LUT already encodes; but the preview
    // column needs the RGB, so recompute via the same blend and convert back.
    const Lab lab = blend.pair_lab(palette[a], palette[b], pct_b / 100.0);
    return lab_to_rgb(lab);
}

// Build the RGB8 preview of a triple recipe.
RGB8 preview_multi(const std::vector<RGB8>& palette, const BlendFns& blend,
                   const std::vector<unsigned>& ids, const std::vector<int>& weights)
{
    std::vector<RGB8> colors;
    std::vector<double> w;
    colors.reserve(ids.size());
    w.reserve(weights.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        colors.push_back(palette[ids[i] - 1]);
        w.push_back(static_cast<double>(weights[i]));
    }
    const Lab lab = blend.multi_lab(colors, w);
    return lab_to_rgb(lab);
}

} // namespace

// ===========================================================================
// CachedSearcher — holds the palette + pair LUT, rebuilds only when the
// palette changes. match_one runs the full search reusing the cached LUT.
// ===========================================================================

CachedSearcher::CachedSearcher(const BlendFns& blend, const std::vector<RGB8>& palette)
    : m_blend(blend), m_palette(palette)
{
    m_palette_lab.resize(m_palette.size());
    for (size_t i = 0; i < m_palette.size(); ++i)
        m_palette_lab[i] = rgb_to_lab(m_palette[i]);
    build_lut();
    // Triple cache uses loop_min=1 (the default floor); match_one will skip
    // cache when a different min_percent is requested (rare).
    build_triple_cache(1);
}

void CachedSearcher::build_lut()
{
    const size_t n = m_palette.size();
    m_pair_lut.assign(n, {});
    for (size_t a = 0; a < n; ++a) {
        m_pair_lut[a].assign(n - a, {});
        for (size_t b = a; b < n; ++b) {
            m_pair_lut[a][b - a].resize(101);
            for (int pct = 0; pct <= 100; ++pct) {
                m_pair_lut[a][b - a][pct] = m_blend.pair_lab(m_palette[a], m_palette[b], pct / 100.0);
            }
        }
    }
}

void CachedSearcher::build_triple_cache(int loop_min)
{
    constexpr int k_step = 10; // triple coarse step
    const size_t n = m_palette.size();
    m_triple_cache.clear();
    if (n < 3) return;

    for (size_t a = 0; a + 2 < n; ++a) {
        for (size_t b = a + 1; b + 1 < n; ++b) {
            for (size_t c = b + 1; c < n; ++c) {
                TripleKey key{ unsigned(a), unsigned(b), unsigned(c) };
                std::vector<TripleEntry> entries;
                std::vector<RGB8> colors = { m_palette[a], m_palette[b], m_palette[c] };
                for (int wa = loop_min; wa <= 100 - 2 * loop_min; wa += k_step) {
                    for (int wb = loop_min; wa + wb <= 100 - loop_min; wb += k_step) {
                        const int wc = 100 - wa - wb;
                        if (wc < loop_min) continue;
                        std::vector<double> w = { double(wa), double(wb), double(wc) };
                        entries.push_back({ wa, wb, wc, m_blend.multi_lab(colors, w) });
                    }
                }
                m_triple_cache.emplace(std::move(key), std::move(entries));
            }
        }
    }
}

MatchResult CachedSearcher::match_one(Lab target_lab,
                                      int min_percent,
                                      const std::vector<std::pair<int, int>>& exclude_pairs) const
{
    MatchResult best;
    const size_t n = m_palette.size();
    if (n < 2) return best;

    const int loop_min = std::max(1, std::clamp(min_percent, 0, 50));

    // cached LUT accessor
    auto lut_get = [&](size_t a, size_t b, int pct) -> const Lab& {
        return m_pair_lut[a][b - a][pct];
    };

    auto update_best_pair = [&](size_t a0, size_t b0, int pct_b, double de) {
        if (!best.valid || de + 1e-6 < best.delta_e) {
            best.valid = true;
            best.type = MatchResult::Type::Pair;
            best.ids = { static_cast<unsigned>(a0 + 1), static_cast<unsigned>(b0 + 1) };
            best.weights = { 100 - pct_b, pct_b };
            best.preview_rgb = preview_pair(m_palette, m_blend, a0, b0, pct_b);
            best.preview_lab = lut_get(a0, b0, pct_b);
            best.delta_e = de;
        }
    };

    // ---- pair coarse scan, step = 5% ----
    constexpr int k_coarse_step = 5;
    constexpr size_t k_top_coarse = 30;

    using HeapEntry = std::tuple<double, unsigned, unsigned, int>;
    auto cmp = [](const HeapEntry& x, const HeapEntry& y) { return std::get<0>(x) < std::get<0>(y); };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> heap(cmp);

    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a + 1; b < n; ++b) {
            if (excluded(a, b, exclude_pairs)) continue;
            for (int pct = loop_min; pct <= 100 - loop_min; pct += k_coarse_step) {
                const Lab& blended = lut_get(a, b, pct);
                const double de = delta_e_2000(target_lab, blended);
                update_best_pair(a, b, pct, de);
                if (heap.size() < k_top_coarse) {
                    heap.emplace(de, static_cast<unsigned>(a + 1), static_cast<unsigned>(b + 1), pct);
                } else if (de < std::get<0>(heap.top())) {
                    heap.pop();
                    heap.emplace(de, static_cast<unsigned>(a + 1), static_cast<unsigned>(b + 1), pct);
                }
            }
        }
    }

    // ---- pair fine scan ----
    while (!heap.empty()) {
        auto [de, a1, b1, coarse_pct] = heap.top();
        heap.pop();
        const size_t a = a1 - 1, b = b1 - 1;
        const int fine_min = std::max(loop_min, coarse_pct - k_coarse_step + 1);
        const int fine_max = std::min(100 - loop_min, coarse_pct + k_coarse_step - 1);
        for (int pct = fine_min; pct <= fine_max; ++pct) {
            if ((pct - loop_min) % k_coarse_step == 0) continue;
            update_best_pair(a, b, pct, delta_e_2000(target_lab, lut_get(a, b, pct)));
        }
    }

    MatchResult best_pair = best;
    if (best_pair.valid && best_pair.delta_e <= 0.5) return best_pair;

    // ---- triple search (needs ≥3 colors) ----
    if (n < 3) return best_pair;

    std::vector<std::pair<double, unsigned>> ranked;
    ranked.reserve(n);
    for (size_t i = 0; i < n; ++i)
        ranked.emplace_back(delta_e_2000(target_lab, m_palette_lab[i]), static_cast<unsigned>(i + 1));
    std::sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) {
        if (x.first != y.first) return x.first < y.first;
        return x.second < y.second;
    });

    const size_t pool_size = std::min<size_t>(n, 8);
    std::vector<unsigned> pool;
    pool.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) pool.push_back(ranked[i].second);
    std::sort(pool.begin(), pool.end());
    if (pool.size() < 3) return best_pair;

    constexpr int k_triple_step = 10;
    constexpr size_t k_top_triple = 20;

    struct TripleEntry { double de; unsigned a, b, c; int wa, wb;
        bool operator<(const TripleEntry& o) const { return de < o.de; } };
    std::priority_queue<TripleEntry> triple_heap;

    best = MatchResult{};

    auto update_best_triple = [&](unsigned a, unsigned b, unsigned c, int wa, int wb, int wc, double de) {
        if (!best.valid || de + 1e-6 < best.delta_e) {
            best.valid = true;
            best.type = MatchResult::Type::Triple;
            best.ids = { a, b, c };
            best.weights = { wa, wb, wc };
            best.preview_rgb = preview_multi(m_palette, m_blend, best.ids, best.weights);
            std::vector<RGB8> colors = { m_palette[a - 1], m_palette[b - 1], m_palette[c - 1] };
            std::vector<double> w = { static_cast<double>(wa), static_cast<double>(wb), static_cast<double>(wc) };
            best.preview_lab = m_blend.multi_lab(colors, w);
            best.delta_e = de;
        }
    };

    auto compat3 = [&](unsigned a, unsigned b, unsigned c) {
        return !excluded(a - 1, b - 1, exclude_pairs) &&
               !excluded(b - 1, c - 1, exclude_pairs) &&
               !excluded(a - 1, c - 1, exclude_pairs);
    };

    for (size_t fi = 0; fi + 2 < pool.size(); ++fi) {
        for (size_t fj = fi + 1; fj + 1 < pool.size(); ++fj) {
            for (size_t fk = fj + 1; fk < pool.size(); ++fk) {
                const unsigned a = pool[fi], b = pool[fj], c = pool[fk];
                if (!compat3(a, b, c)) continue;
                // Use cached coarse Labs if available (loop_min == 1, the common case).
                const bool use_cache = (loop_min == 1);
                if (use_cache) {
                    auto it = m_triple_cache.find(TripleKey{a - 1, b - 1, c - 1});
                    if (it != m_triple_cache.end()) {
                        for (const auto& e : it->second) {
                            const double de = delta_e_2000(target_lab, e.lab);
                            update_best_triple(a, b, c, e.wa, e.wb, e.wc, de);
                            if (triple_heap.size() < k_top_triple) triple_heap.push({ de, a, b, c, e.wa, e.wb });
                            else if (de < triple_heap.top().de) { triple_heap.pop(); triple_heap.push({ de, a, b, c, e.wa, e.wb }); }
                        }
                        continue;
                    }
                }
                // Fallback: compute on the fly (non-default loop_min or cache miss).
                for (int wa = loop_min; wa <= 100 - 2 * loop_min; wa += k_triple_step) {
                    for (int wb = loop_min; wa + wb <= 100 - loop_min; wb += k_triple_step) {
                        const int wc = 100 - wa - wb;
                        if (wc < loop_min) continue;
                        std::vector<RGB8> colors = { m_palette[a - 1], m_palette[b - 1], m_palette[c - 1] };
                        std::vector<double> w = { static_cast<double>(wa), static_cast<double>(wb), static_cast<double>(wc) };
                        const Lab blended = m_blend.multi_lab(colors, w);
                        const double de = delta_e_2000(target_lab, blended);
                        update_best_triple(a, b, c, wa, wb, wc, de);
                        if (triple_heap.size() < k_top_triple) triple_heap.push({ de, a, b, c, wa, wb });
                        else if (de < triple_heap.top().de) { triple_heap.pop(); triple_heap.push({ de, a, b, c, wa, wb }); }
                    }
                }
            }
        }
    }

    while (!triple_heap.empty()) {
        const TripleEntry te = triple_heap.top();
        triple_heap.pop();
        // Fine window: ±4 around the coarse center (tighter than the slicer's
        // ±9 to cut the multi_lab cost ~4x; ΔE impact is negligible since the
        // coarse 10% grid already localizes the optimum).
        const int fine_half = 4;
        const int wa_min = std::max(loop_min, te.wa - fine_half);
        const int wa_max = std::min(100 - 2 * loop_min, te.wa + fine_half);
        for (int wa = wa_min; wa <= wa_max; ++wa) {
            if (wa % k_triple_step == 0) continue; // coarse point already evaluated
            const int wb_min = std::max(loop_min, te.wb - fine_half);
            const int wb_max = std::min(100 - wa - loop_min, te.wb + fine_half);
            for (int wb = wb_min; wb <= wb_max; ++wb) {
                if (wb % k_triple_step == 0) continue; // coarse point already evaluated
                const int wc = 100 - wa - wb;
                if (wc < loop_min) continue;
                std::vector<RGB8> colors = { m_palette[te.a - 1], m_palette[te.b - 1], m_palette[te.c - 1] };
                std::vector<double> w = { static_cast<double>(wa), static_cast<double>(wb), static_cast<double>(wc) };
                update_best_triple(te.a, te.b, te.c, wa, wb, wc, delta_e_2000(target_lab, m_blend.multi_lab(colors, w)));
            }
        }
    }

    // final normalization
    if (best.valid) {
        best.preview_lab = rgb_to_lab(best.preview_rgb);
        best.delta_e = delta_e_2000(target_lab, best.preview_lab);
    }
    if (best_pair.valid) {
        best_pair.preview_lab = rgb_to_lab(best_pair.preview_rgb);
        best_pair.delta_e = delta_e_2000(target_lab, best_pair.preview_lab);
        if (!best.valid || best_pair.delta_e + 1e-6 < best.delta_e) best = std::move(best_pair);
        else if (best.type == MatchResult::Type::Triple && best_pair.delta_e <= best.delta_e + 0.5)
            best = std::move(best_pair);
    } else {
        best = std::move(best_pair);
    }
    return best;
}

// Free function wrapper: builds a searcher for one-off use (batch CLI mode).
MatchResult search_best(const std::vector<RGB8>& palette,
                        Lab target_lab,
                        const BlendFns& blend,
                        int min_percent,
                        const std::vector<std::pair<int, int>>& exclude_pairs)
{
    CachedSearcher s(blend, palette);
    return s.match_one(target_lab, min_percent, exclude_pairs);
}

} // namespace cmb
