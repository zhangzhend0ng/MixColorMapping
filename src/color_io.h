// color_io.h — color format parsing / formatting for the batch matcher.
//
// All Lab and ΔE2000 math is forwarded to the vendored prusa_fdm_mixer model
// (the same yardstick used by the slicer's A/B test), so the two algorithms
// under comparison are scored on identical ground.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cmb {

// 8-bit sRGB color — the universal internal representation. Both vendored
// algorithms operate on uint8 channels, so every input format normalizes to
// this before any blending.
struct RGB8 {
    std::uint8_t r = 0, g = 0, b = 0;
};

// CIELAB (D65). L ∈ [0,100], a/b typically ∈ [-128,127].
struct Lab {
    double L = 0.0, a = 0.0, b = 0.0;
};

// Scale hint for parsing numeric RGB columns.
enum class RgbScale {
    Auto,   // any channel ≥ 1.5 ⇒ treat as 0-255, else 0-1
    Range255,
    Range01,
};

// The four supported per-table color encodings.
enum class ColorFormat {
    Hex,
    RGB,
    Lab,
    CMYK,
};

// ---- hex ----

// Parse "#RRGGBB" (or "RRGGBB", or with trailing alpha "#RRGGBBAA" — alpha
// ignored, matching the slicer's defensive hex_to_rgb). Returns false on
// malformed input instead of throwing.
bool parse_hex(const std::string& s, RGB8& out);

// Format as "#rrggbb" lowercase.
std::string to_hex(RGB8 c);

// ---- rgb ----

// Build an RGB8 from three numeric channels using the given scale hint.
RGB8 parse_rgb(double r, double g, double b, RgbScale scale);

// Decide scale from raw values (used by RgbScale::Auto).
RgbScale detect_rgb_scale(double r, double g, double b);

// ---- lab ----

RGB8 lab_to_rgb(const Lab& lab);
Lab  rgb_to_lab(RGB8 c);

// ΔE2000 between two Lab colors.
double delta_e_2000(const Lab& a, const Lab& b);

// ---- cmyk ----

// Simple device-CMYK ↔ sRGB. Inputs/outputs are fractions in [0,1].
//   R = 255 * (1 - C) * (1 - K),  etc.
RGB8 cmyk_to_rgb(double C, double M, double Y, double K);
void rgb_to_cmyk(RGB8 c, double& C, double& M, double& Y, double& K);

// ---- parsing helpers for CSV rows ----

// Split a CSV line into trimmed fields. Handles optional quoting and CRLF.
std::vector<std::string> split_csv(const std::string& line);

// Parse a single color row given the table's format. The first column is
// always the label; remaining columns are the color components.
//   hex:  label,#RRGGBB
//   rgb:  label,R,G,B
//   lab:  label,L,a,b
//   cmyk: label,C,M,Y,K
// Returns false on parse failure (out_label may still be partially filled).
bool parse_color_row(const std::vector<std::string>& fields,
                     ColorFormat fmt,
                     RgbScale rgb_scale,
                     std::string& out_label,
                     RGB8& out_color);

// ---- output formatting ----

// Format a single channel value for CSV: integers without decimals,
// Lab/CMYK with fixed precision.
std::string fmt_int(int v);
std::string fmt_double(double v, int precision = 4);

} // namespace cmb
