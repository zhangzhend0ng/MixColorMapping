// test_golden.cpp — regression checks pinning the vendored algorithms to
// known-good values from the slicer. Standalone (no test framework): exit
// code 0 = all pass, 1 = at least one failure. Run via `tests/run_tests.sh`
// or compile directly.
//
// These mirror:
//   - tests/libslic3r/test_mixed_filament_color_golden.cpp in the slicer
//     (FS polynomial endpoints + documented blue+yellow example).
//   - the gradient-safety invariant from test_mix_model_ab_comparison.cpp.
#include "filament_mixer.h"
#include "prusa_fdm_mixer.hpp"

#include <cstdio>
#include <cstdlib>

static int g_failures = 0;

#define CHECK_EQ(label, got, expected)                                        \
    do {                                                                      \
        const auto _g = (got);                                                \
        const auto _e = (expected);                                           \
        if (_g == _e) {                                                       \
            std::printf("[PASS] %s\n", label);                                \
        } else {                                                              \
            std::printf("[FAIL] %s  got=%d expected=%d\n", label, (int)_g, (int)_e); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

int main()
{
    // ---- FS golden: blue+yellow @ t=0.5 → (47,141,56) ----
    // This is the exact value pinned by the slicer's golden test, verifying
    // the polynomial model was vendored intact.
    {
        unsigned char r = 0, g = 0, b = 0;
        Slic3r::filament_mixer_lerp(0, 33, 133, 252, 211, 0, 0.5f, &r, &g, &b);
        CHECK_EQ("FS blue+yellow r", r, 47);
        CHECK_EQ("FS blue+yellow g", g, 141);
        CHECK_EQ("FS blue+yellow b", b, 56);
    }

    // ---- FS endpoints ----
    {
        unsigned char r = 0, g = 0, b = 0;
        Slic3r::filament_mixer_lerp(10, 20, 30, 200, 210, 220, 0.0f, &r, &g, &b);
        CHECK_EQ("FS t=0 r", r, 10);
        CHECK_EQ("FS t=0 g", g, 20);
        CHECK_EQ("FS t=0 b", b, 30);

        Slic3r::filament_mixer_lerp(10, 20, 30, 200, 210, 220, 1.0f, &r, &g, &b);
        CHECK_EQ("FS t=1 r", r, 200);
        CHECK_EQ("FS t=1 g", g, 210);
        CHECK_EQ("FS t=1 b", b, 220);
    }

    // ---- Prusa gradient-safety: ratio ≥ 0.9999 returns source exactly ----
    {
        const auto pr = prusa_fdm_mixer::mix_rgb({ {"#ff0000", 1.0}, {"#00ff00", 0.0} });
        CHECK_EQ("Prusa ratio=1 r", pr.r, 255);
        CHECK_EQ("Prusa ratio=1 g", pr.g, 0);
        CHECK_EQ("Prusa ratio=1 b", pr.b, 0);
    }

    // ---- Prusa ΔE2000 self-consistency: same color → 0 ----
    {
        const auto L1 = prusa_fdm_mixer::rgb_to_lab({255, 0, 0});
        const auto L2 = prusa_fdm_mixer::rgb_to_lab({255, 0, 0});
        const double de = prusa_fdm_mixer::delta_e_2000(L1, L2);
        if (de < 0.0001) {
            std::printf("[PASS] Prusa deltaE(same)=0\n");
        } else {
            std::printf("[FAIL] Prusa deltaE(same)=%.6f expected 0\n", de);
            ++g_failures;
        }
    }

    // ---- Prusa hex_to_rgb tolerates #RRGGBBAA (slicer patch) ----
    {
        const auto pr = prusa_fdm_mixer::hex_to_rgb("#ff8800ff"); // trailing alpha
        CHECK_EQ("Prusa hex #RRGGBBAA r", pr.r, 255);
        CHECK_EQ("Prusa hex #RRGGBBAA g", pr.g, 136);
        CHECK_EQ("Prusa hex #RRGGBBAA b", pr.b, 0);
    }

    if (g_failures == 0) {
        std::printf("\nAll golden checks passed.\n");
        return 0;
    }
    std::printf("\n%d golden check(s) FAILED.\n", g_failures);
    return 1;
}
