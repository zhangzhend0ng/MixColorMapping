// color_io.cpp — see color_io.h.
#include "color_io.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "prusa_fdm_mixer.hpp" // vendored: rgb_to_lab / lab_to_rgb / delta_e_2000

namespace cmb {

namespace {

inline bool is_hex_digit(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

inline int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return ch - 'A' + 10;
}

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

// ---------------------------------------------------------------------------
// hex
// ---------------------------------------------------------------------------

bool parse_hex(const std::string& s_in, RGB8& out)
{
    std::string s = trim(s_in);
    if (s.empty()) return false;
    if (s[0] == '#') s.erase(0, 1);
    if (s.size() < 6) return false;
    for (int i = 0; i < 6; ++i)
        if (!is_hex_digit(s[i])) return false;

    out.r = static_cast<std::uint8_t>((hex_val(s[0]) << 4) | hex_val(s[1]));
    out.g = static_cast<std::uint8_t>((hex_val(s[2]) << 4) | hex_val(s[3]));
    out.b = static_cast<std::uint8_t>((hex_val(s[4]) << 4) | hex_val(s[5]));
    return true;
}

std::string to_hex(RGB8 c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

// ---------------------------------------------------------------------------
// rgb
// ---------------------------------------------------------------------------

RgbScale detect_rgb_scale(double r, double g, double b)
{
    // If any channel clearly exceeds the [0,1] range, assume 0-255.
    if (r >= 1.5 || g >= 1.5 || b >= 1.5) return RgbScale::Range255;
    // Values in [0,1] are ambiguous (could be tiny 0-255). We resolve this by
    // treating the whole triplet as 0-1 only when ALL channels are ≤ 1; the
    // caller can force the interpretation with --rgb-scale.
    if (r <= 1.0 && g <= 1.0 && b <= 1.0) return RgbScale::Range01;
    return RgbScale::Range255;
}

RGB8 parse_rgb(double r, double g, double b, RgbScale scale)
{
    if (scale == RgbScale::Auto) scale = detect_rgb_scale(r, g, b);

    auto clamp8 = [](double v) -> std::uint8_t {
        if (v < 0.0) v = 0.0;
        if (v > 255.0) v = 255.0;
        return static_cast<std::uint8_t>(std::lround(v));
    };

    if (scale == RgbScale::Range01) {
        return RGB8{ clamp8(r * 255.0), clamp8(g * 255.0), clamp8(b * 255.0) };
    }
    return RGB8{ clamp8(r), clamp8(g), clamp8(b) };
}

// ---------------------------------------------------------------------------
// lab — forwarded to the vendored prusa model
// ---------------------------------------------------------------------------

RGB8 lab_to_rgb(const Lab& lab)
{
    prusa_fdm_mixer::LAB pl{ lab.L, lab.a, lab.b };
    prusa_fdm_mixer::RGB pr = prusa_fdm_mixer::lab_to_rgb(pl);
    return RGB8{ pr.r, pr.g, pr.b };
}

Lab rgb_to_lab(RGB8 c)
{
    prusa_fdm_mixer::RGB pr{ c.r, c.g, c.b };
    prusa_fdm_mixer::LAB pl = prusa_fdm_mixer::rgb_to_lab(pr);
    return Lab{ pl.L, pl.a, pl.b };
}

double delta_e_2000(const Lab& a, const Lab& b)
{
    prusa_fdm_mixer::LAB pa{ a.L, a.a, a.b };
    prusa_fdm_mixer::LAB pb{ b.L, b.a, b.b };
    return prusa_fdm_mixer::delta_e_2000(pa, pb);
}

// ---------------------------------------------------------------------------
// cmyk — simple device conversion
// ---------------------------------------------------------------------------

RGB8 cmyk_to_rgb(double C, double M, double Y, double K)
{
    auto cl = [](double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); };
    C = cl(C); M = cl(M); Y = cl(Y); K = cl(K);
    // Standard device-CMYK formula.
    const double r = (1.0 - C) * (1.0 - K);
    const double g = (1.0 - M) * (1.0 - K);
    const double b = (1.0 - Y) * (1.0 - K);
    auto u8 = [](double v) -> std::uint8_t {
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        return static_cast<std::uint8_t>(std::lround(v * 255.0));
    };
    return RGB8{ u8(r), u8(g), u8(b) };
}

void rgb_to_cmyk(RGB8 c, double& C, double& M, double& Y, double& K)
{
    const double r = c.r / 255.0;
    const double g = c.g / 255.0;
    const double b = c.b / 255.0;
    K = 1.0 - std::max({ r, g, b });
    if (K >= 1.0) { C = M = Y = 0.0; return; } // pure black
    const double ik = 1.0 - K;
    C = (ik - r) / ik;
    M = (ik - g) / ik;
    Y = (ik - b) / ik;
}

// ---------------------------------------------------------------------------
// CSV row helpers
// ---------------------------------------------------------------------------

std::vector<std::string> split_csv(const std::string& line)
{
    std::vector<std::string> out;
    std::string field;
    bool in_quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (in_quote) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { field.push_back('"'); ++i; }
                else in_quote = false;
            } else {
                field.push_back(ch);
            }
        } else {
            if (ch == '"') in_quote = true;
            else if (ch == ',') { out.push_back(field); field.clear(); }
            else field.push_back(ch);
        }
    }
    out.push_back(field);
    // Trim each field.
    for (auto& f : out) f = trim(f);
    return out;
}

bool parse_double_str(const std::string& s, double& out)
{
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        out = std::stod(s, &pos);
        // allow trailing whitespace (already trimmed, but be safe)
        return pos > 0;
    } catch (...) {
        return false;
    }
}

bool parse_color_row(const std::vector<std::string>& fields,
                     ColorFormat fmt,
                     RgbScale rgb_scale,
                     std::string& out_label,
                     RGB8& out_color)
{
    if (fields.empty()) return false;
    out_label = fields[0];

    switch (fmt) {
    case ColorFormat::Hex: {
        if (fields.size() < 2) return false;
        return parse_hex(fields[1], out_color);
    }
    case ColorFormat::RGB: {
        if (fields.size() < 4) return false;
        double r, g, b;
        if (!parse_double_str(fields[1], r) ||
            !parse_double_str(fields[2], g) ||
            !parse_double_str(fields[3], b)) return false;
        out_color = parse_rgb(r, g, b, rgb_scale);
        return true;
    }
    case ColorFormat::Lab: {
        if (fields.size() < 4) return false;
        double L, a, b;
        if (!parse_double_str(fields[1], L) ||
            !parse_double_str(fields[2], a) ||
            !parse_double_str(fields[3], b)) return false;
        out_color = lab_to_rgb(Lab{ L, a, b });
        return true;
    }
    case ColorFormat::CMYK: {
        if (fields.size() < 5) return false;
        double C, M, Y, K;
        if (!parse_double_str(fields[1], C) ||
            !parse_double_str(fields[2], M) ||
            !parse_double_str(fields[3], Y) ||
            !parse_double_str(fields[4], K)) return false;
        // CMYK in spreadsheets is commonly 0-100; normalize to fractions.
        // Heuristic: if any value > 1.5, treat all as percent.
        if (C > 1.5 || M > 1.5 || Y > 1.5 || K > 1.5) {
            C /= 100.0; M /= 100.0; Y /= 100.0; K /= 100.0;
        }
        out_color = cmyk_to_rgb(C, M, Y, K);
        return true;
    }
    }
    return false;
}

// ---------------------------------------------------------------------------
// output formatting
// ---------------------------------------------------------------------------

std::string fmt_int(int v)
{
    return std::to_string(v);
}

std::string fmt_double(double v, int precision)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return buf;
}

} // namespace cmb
