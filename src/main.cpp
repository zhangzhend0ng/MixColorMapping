// main.cpp — color_match_batch CLI entry point.
//
// Reads a palette CSV and a targets CSV, runs the FS and Prusa reverse-match
// searches for every target, and writes a single results CSV with both
// algorithms' best recipes side by side. See README.md for column layout.
#include "color_io.h"
#include "match_search.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void print_usage()
{
    std::cerr <<
        "color_match_batch — FS vs Prusa color-mix reverse match\n"
        "\n"
        "USAGE:\n"
        "  color_match_batch --palette P.csv --targets T.csv --output R.csv \\\n"
        "                    --input-format hex|rgb|lab|cmyk [options]\n"
        "\n"
        "REQUIRED:\n"
        "  --palette <file>        CSV of physical filament colors (hex or \"#hex,label\")\n"
        "  --targets <file>        CSV of target colors (first column = label)\n"
        "  --output  <file>        Output CSV path\n"
        "  --input-format <fmt>    hex | rgb | lab | cmyk   (applies to targets)\n"
        "\n"
        "OPTIONS:\n"
        "  --min-percent <n>       Minimum per-component percent (0-50, default 0)\n"
        "  --exclude \"1-2,3-4\"     1-based palette-id pairs to skip (incompatible)\n"
        "  --rgb-scale auto|255|1  How to read numeric RGB columns (default auto)\n"
        "  -h, --help              Show this message\n";
}

struct CliArgs {
    std::string palette_path;
    std::string targets_path;
    std::string output_path;
    cmb::ColorFormat fmt = cmb::ColorFormat::Hex;
    cmb::RgbScale rgb_scale = cmb::RgbScale::Auto;
    int min_percent = 0;
    std::vector<std::pair<int, int>> exclude;
    bool ok = false;
};

bool parse_args(int argc, char** argv, CliArgs& out)
{
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) { std::cerr << "error: missing value for " << argv[i] << "\n"; return nullptr; }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_usage(); return false; }
        else if (arg == "--palette") { if (const char* v = need(i)) out.palette_path = v; else return false; }
        else if (arg == "--targets") { if (const char* v = need(i)) out.targets_path = v; else return false; }
        else if (arg == "--output")  { if (const char* v = need(i)) out.output_path  = v; else return false; }
        else if (arg == "--input-format") {
            const char* v = need(i); if (!v) return false;
            const std::string s = v;
            if      (s == "hex")  out.fmt = cmb::ColorFormat::Hex;
            else if (s == "rgb")  out.fmt = cmb::ColorFormat::RGB;
            else if (s == "lab")  out.fmt = cmb::ColorFormat::Lab;
            else if (s == "cmyk") out.fmt = cmb::ColorFormat::CMYK;
            else { std::cerr << "error: --input-format must be hex|rgb|lab|cmyk\n"; return false; }
        }
        else if (arg == "--rgb-scale") {
            const char* v = need(i); if (!v) return false;
            const std::string s = v;
            if      (s == "auto") out.rgb_scale = cmb::RgbScale::Auto;
            else if (s == "255")  out.rgb_scale = cmb::RgbScale::Range255;
            else if (s == "1")    out.rgb_scale = cmb::RgbScale::Range01;
            else { std::cerr << "error: --rgb-scale must be auto|255|1\n"; return false; }
        }
        else if (arg == "--min-percent") {
            const char* v = need(i); if (!v) return false;
            try { out.min_percent = std::stoi(v); }
            catch (...) { std::cerr << "error: --min-percent must be an integer\n"; return false; }
        }
        else if (arg == "--exclude") {
            const char* v = need(i); if (!v) return false;
            // parse "1-2,3-4"
            std::stringstream ss(v);
            std::string token;
            while (std::getline(ss, token, ',')) {
                const size_t dash = token.find('-');
                if (dash == std::string::npos) {
                    std::cerr << "error: bad --exclude pair \"" << token << "\" (expected a-b)\n";
                    return false;
                }
                try {
                    const int a = std::stoi(token.substr(0, dash));
                    const int b = std::stoi(token.substr(dash + 1));
                    out.exclude.emplace_back(a, b);
                } catch (...) {
                    std::cerr << "error: bad --exclude pair \"" << token << "\"\n";
                    return false;
                }
            }
        }
        else {
            std::cerr << "error: unknown argument \"" << arg << "\"\n";
            print_usage();
            return false;
        }
    }

    if (out.palette_path.empty() || out.targets_path.empty() || out.output_path.empty()) {
        std::cerr << "error: --palette, --targets, --output are all required\n";
        return false;
    }
    out.ok = true;
    return true;
}

// Read a CSV file into rows of fields, skipping blank lines and lines that
// start with '#' (treated as comments — except the palette's hex values, which
// begin with '#' inside a quoted/second field, handled after split).
std::vector<std::vector<std::string>> read_csv_rows(const std::string& path)
{
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(path);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        return rows;
    }
    std::string line;
    while (std::getline(in, line)) {
        // skip completely empty lines
        bool only_ws = true;
        for (char c : line) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { only_ws = false; break; }
        if (only_ws) continue;
        rows.push_back(cmb::split_csv(line));
    }
    return rows;
}

bool load_palette(const std::string& path, std::vector<cmb::RGB8>& palette, std::vector<std::string>& labels)
{
    const auto rows = read_csv_rows(path);
    if (rows.empty()) {
        std::cerr << "error: palette file is empty or unreadable: " << path << "\n";
        return false;
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& fields = rows[i];
        if (fields.empty()) continue;

        // Detect and skip a header row: first field not a hex color.
        cmb::RGB8 rgb;
        const std::string& first = fields[0];
        // Palette rows may be either "#hex" alone, or "#hex,label", or "label,#hex".
        // Try first field as hex, else second field.
        bool parsed = false;
        std::string label;
        if (cmb::parse_hex(first, rgb)) {
            parsed = true;
            label = fields.size() > 1 ? fields[1] : ("color_" + std::to_string(palette.size() + 1));
        } else if (fields.size() > 1 && cmb::parse_hex(fields[1], rgb)) {
            parsed = true;
            label = first;
        }
        if (!parsed) {
            if (i == 0) continue; // header
            std::cerr << "warning: palette row " << (i + 1) << " not a hex color, skipping\n";
            continue;
        }
        palette.push_back(rgb);
        labels.push_back(label);
    }
    if (palette.size() < 2) {
        std::cerr << "error: palette needs at least 2 colors, found " << palette.size() << "\n";
        return false;
    }
    return true;
}

// Emit one color's full representation as CSV columns (hex,RGB,Lab,CMYK = 11 cols).
void emit_color_columns(std::ostream& os, cmb::RGB8 c)
{
    const cmb::Lab lab = cmb::rgb_to_lab(c);
    double C, M, Y, K;
    cmb::rgb_to_cmyk(c, C, M, Y, K);
    os << cmb::to_hex(c) << ","
       << (int)c.r << "," << (int)c.g << "," << (int)c.b << ","
       << cmb::fmt_double(lab.L) << "," << cmb::fmt_double(lab.a) << "," << cmb::fmt_double(lab.b) << ","
       << cmb::fmt_double(C * 100.0) << "," << cmb::fmt_double(M * 100.0) << ","
       << cmb::fmt_double(Y * 100.0) << "," << cmb::fmt_double(K * 100.0);
}

// Header for one recipe's block. Always emits the full fixed column set so the
// header row stays aligned regardless of whether the dummy result is valid.
// Layout: <p>_type, <p>_ids, <p>_weights,
//          <p>_hex, <p>_R, <p>_G, <p>_B,
//          <p>_L, <p>_a, <p>_b,
//          <p>_C, <p>_M, <p>_Y, <p>_K,
//          <p>_delta_e
void emit_recipe_columns(std::ostream& os, const std::string& prefix)
{
    os << prefix << "_type," << prefix << "_ids," << prefix << "_weights,"
       << prefix << "_hex," << prefix << "_R," << prefix << "_G," << prefix << "_B,"
       << prefix << "_L," << prefix << "_a," << prefix << "_b,"
       << prefix << "_C," << prefix << "_M," << prefix << "_Y," << prefix << "_K,"
       << prefix << "_delta_e";
}

// Values for one recipe's block, matching emit_recipe_columns column-for-column.
void emit_recipe_values(std::ostream& os, const cmb::MatchResult& r)
{
    const char* type_str = !r.valid ? "none" : (r.type == cmb::MatchResult::Type::Pair ? "pair" : "triple");
    os << type_str << ",";

    std::string ids, weights;
    for (size_t i = 0; i < r.ids.size(); ++i) { if (i) ids += "/"; ids += std::to_string(r.ids[i]); }
    for (size_t i = 0; i < r.weights.size(); ++i) { if (i) weights += "/"; weights += std::to_string(r.weights[i]); }
    os << ids << "," << weights << ",";

    if (r.valid) {
        emit_color_columns(os, r.preview_rgb);
        os << "," << cmb::fmt_double(r.delta_e);
    } else {
        // 12 empty cells: hex,R,G,B,L,a,b,C,M,Y,K,delta_e
        for (int i = 0; i < 12; ++i) { if (i) os << ","; }
    }
}

} // namespace

int main(int argc, char** argv)
{
    CliArgs args;
    if (!parse_args(argc, argv, args) || !args.ok) {
        return args.palette_path.empty() ? 1 : 0; // --help returns 0
    }

    // ---- load palette ----
    std::vector<cmb::RGB8> palette;
    std::vector<std::string> palette_labels;
    if (!load_palette(args.palette_path, palette, palette_labels)) return 1;

    std::cerr << "palette: " << palette.size() << " colors loaded\n";
    for (size_t i = 0; i < palette.size(); ++i) {
        std::cerr << "  [" << (i + 1) << "] " << cmb::to_hex(palette[i])
                  << "  " << palette_labels[i] << "\n";
    }

    // ---- load targets ----
    const auto target_rows = read_csv_rows(args.targets_path);
    if (target_rows.empty()) {
        std::cerr << "error: targets file is empty or unreadable: " << args.targets_path << "\n";
        return 1;
    }

    // ---- blend backends ----
    const cmb::BlendFns fs = cmb::fs_blend();
    const cmb::BlendFns pr = cmb::prusa_blend();

    // ---- open output ----
    std::ofstream out(args.output_path);
    if (!out) {
        std::cerr << "error: cannot write to " << args.output_path << "\n";
        return 1;
    }

    // ---- header ----
    out << "label,";
    out << "target_hex,target_R,target_G,target_B,"
        << "target_L,target_a,target_b,"
        << "target_C,target_M,target_Y,target_K,";
    emit_recipe_columns(out, "fs");     out << ",";
    emit_recipe_columns(out, "prusa");  out << "\n";

    // ---- process each target ----
    size_t processed = 0, skipped = 0;
    for (size_t ri = 0; ri < target_rows.size(); ++ri) {
        const auto& fields = target_rows[ri];
        std::string label;
        cmb::RGB8 target_rgb;
        if (!cmb::parse_color_row(fields, args.fmt, args.rgb_scale, label, target_rgb)) {
            // skip header / malformed
            if (ri == 0) {
                // likely a header row — silently skip
            } else {
                std::cerr << "warning: target row " << (ri + 1) << " failed to parse, skipping\n";
                ++skipped;
            }
            continue;
        }

        const cmb::Lab target_lab = cmb::rgb_to_lab(target_rgb);

        const cmb::MatchResult fs_result = cmb::search_best(palette, target_lab, fs, args.min_percent, args.exclude);
        const cmb::MatchResult pr_result  = cmb::search_best(palette, target_lab, pr, args.min_percent, args.exclude);

        std::cerr << "[" << (processed + 1) << "] " << label
                  << " (" << cmb::to_hex(target_rgb) << ")"
                  << "  fs ΔE=" << cmb::fmt_double(fs_result.delta_e, 3)
                  << "  prusa ΔE=" << cmb::fmt_double(pr_result.delta_e, 3) << "\n";

        // ---- write row ----
        out << label << ",";
        emit_color_columns(out, target_rgb);
        out << ",";
        emit_recipe_values(out, fs_result);
        out << ",";
        emit_recipe_values(out, pr_result);
        out << "\n";

        ++processed;
    }

    std::cerr << "\ndone: " << processed << " targets matched, " << skipped << " skipped"
              << " → " << args.output_path << "\n";
    return 0;
}
