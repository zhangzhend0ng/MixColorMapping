// main.cpp — color_match_batch CLI entry point.
//
// Two modes:
//   1. Batch CSV mode (default): --palette P.csv --targets T.csv --output R.csv
//   2. Interactive serve mode: --serve
//      Reads one JSON request per line from stdin, writes one JSON response per
//      line to stdout. Keeps the process alive so callers (e.g. the web server)
//      avoid the ~50ms process-spawn cost per query.
//
// Serve-mode request (single line JSON):
//   {"palette":[["#ff0000","Red"],...], "targets":[["label","#hex"],...],
//    "format":"hex","min_percent":0,"exclude":"","rgb_scale":"auto"}
// Serve-mode response (single line JSON):
//   {"rows":[{...}],"count":N}  or  {"error":"..."}
#include "color_io.h"
#include "match_search.h"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// =========================================================================
// JSON parsing — minimal hand-rolled parser (no external deps).
// Supports just enough for our request shape: objects, arrays, strings,
// numbers, booleans, null. Throws std::runtime_error on malformed input.
// =========================================================================

class JsonParser {
public:
    JsonParser(const std::string& s) : m_s(s), m_pos(0) {}

    // entry: parse a full JSON value.
    std::string parse_value_as_json() {
        skip_ws();
        if (at_end()) throw std::runtime_error("unexpected end");
        char c = peek();
        size_t start = m_pos;
        if (c == '{') skip_object();
        else if (c == '[') skip_array();
        else if (c == '"') skip_string();
        else if (c == 't' || c == 'f' || c == 'n' ||
                 (c >= '0' && c <= '9') || c == '-') skip_number_or_lit();
        else throw std::runtime_error(std::string("bad json char '") + c + "'");
        return m_s.substr(start, m_pos - start);
    }

    // Find the value at a top-level object key. Returns the raw JSON substring
    // for that value, or "" if key absent. Always rescans from the start so it
    // is safe to call multiple times on the same parser.
    std::string get_key(const std::string& key) {
        m_pos = 0;
        skip_ws();
        if (peek() != '{') throw std::runtime_error("expected object");
        ++m_pos; // {
        skip_ws();
        if (peek() == '}') { ++m_pos; return ""; }
        while (true) {
            skip_ws();
            std::string k = parse_string_value();
            skip_ws();
            if (peek() != ':') throw std::runtime_error("expected ':'");
            ++m_pos; // :
            skip_ws();
            if (k == key) return parse_value_as_json();
            // skip this value
            parse_value_as_json();
            skip_ws();
            char c = next();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("expected ',' or '}'");
        }
        return "";
    }

    // Parse a JSON array of arrays of strings, e.g. [["a","b"],["c"]].
    std::vector<std::vector<std::string>> parse_string_matrix(const std::string& json) {
        JsonParser p(json);
        return p.do_parse_string_matrix();
    }

    // Parse a JSON string literal value (returns unescaped content).
    std::string parse_string_value() {
        skip_ws();
        if (peek() != '"') throw std::runtime_error("expected string");
        return do_parse_string();
    }

    // Parse a number/bool/null literal.
    std::string parse_literal() {
        skip_ws();
        size_t start = m_pos;
        while (!at_end() && peek() != ',' && peek() != '}' && peek() != ']'
               && peek() != ' ' && peek() != '\t' && peek() != '\n' && peek() != '\r')
            ++m_pos;
        return m_s.substr(start, m_pos - start);
    }

private:
    const std::string& m_s;
    size_t m_pos;

    bool at_end() const { return m_pos >= m_s.size(); }
    char peek() const { return at_end() ? '\0' : m_s[m_pos]; }
    char next() { return at_end() ? '\0' : m_s[m_pos++]; }
    void skip_ws() { while (!at_end() && (peek()==' '||peek()=='\t'||peek()=='\n'||peek()=='\r')) ++m_pos; }

    void skip_object() {
        // assumes peek() == '{'
        ++m_pos;
        skip_ws();
        if (peek() == '}') { ++m_pos; return; }
        while (true) {
            skip_ws(); do_parse_string(); // key
            skip_ws();
            if (next() != ':') throw std::runtime_error("expected ':' in object");
            parse_value_as_json(); // value (recurses)
            skip_ws();
            char c = next();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("expected ',' or '}'");
        }
    }
    void skip_array() {
        ++m_pos; // [
        skip_ws();
        if (peek() == ']') { ++m_pos; return; }
        while (true) {
            parse_value_as_json();
            skip_ws();
            char c = next();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("expected ',' or ']'");
        }
    }
    void skip_string() { do_parse_string(); }
    void skip_number_or_lit() {
        while (!at_end()) {
            char c = peek();
            if (c==','||c=='}'||c==']'||c==' '||c=='\t'||c=='\n'||c=='\r') break;
            ++m_pos;
        }
    }

    std::string do_parse_string() {
        if (next() != '"') throw std::runtime_error("expected '\"'");
        std::string out;
        while (!at_end()) {
            char c = next();
            if (c == '"') return out;
            if (c == '\\') {
                char e = next();
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        // skip 4 hex digits, output as-is (latin1 approximation)
                        for (int i = 0; i < 4; ++i) ++m_pos;
                        out += '?';
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        throw std::runtime_error("unterminated string");
    }

    std::vector<std::vector<std::string>> do_parse_string_matrix() {
        skip_ws();
        std::vector<std::vector<std::string>> result;
        if (next() != '[') throw std::runtime_error("expected '['");
        skip_ws();
        if (peek() == ']') { ++m_pos; return result; }
        while (true) {
            skip_ws();
            std::vector<std::string> row;
            if (next() != '[') throw std::runtime_error("expected inner '['");
            skip_ws();
            if (peek() == ']') { ++m_pos; }
            else {
                while (true) {
                    row.push_back(do_parse_string());
                    skip_ws();
                    char c = next();
                    if (c == ']') break;
                    if (c != ',') throw std::runtime_error("expected ',' or ']' in inner array");
                    skip_ws();
                }
            }
            result.push_back(std::move(row));
            skip_ws();
            char c = next();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("expected ',' or ']' in outer array");
        }
        return result;
    }
};

// =========================================================================
// JSON string escaping for output.
// =========================================================================
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// =========================================================================
// Core match logic — shared by CSV mode and serve mode.
// Returns the results as a vector of MatchResult pairs (fs, prusa) + target info.
// =========================================================================
struct TargetInput {
    std::string label;
    std::vector<std::string> fields; // raw CSV-style fields after the label
};

struct MatchOutput {
    std::string label;
    cmb::RGB8 target_rgb;
    cmb::Lab target_lab;
    cmb::MatchResult fs;
    cmb::MatchResult prusa;
};

std::vector<MatchOutput> run_match_for_targets(
    const std::vector<cmb::RGB8>& palette,
    const std::vector<TargetInput>& targets,
    cmb::ColorFormat fmt,
    cmb::RgbScale rgb_scale,
    int min_percent,
    const std::vector<std::pair<int, int>>& exclude)
{
    const cmb::BlendFns fs = cmb::fs_blend();
    const cmb::BlendFns pr = cmb::prusa_blend();

    // Build the pair-LUT ONCE per (palette, algorithm); reuse across all targets.
    // This is the key perf win: the LUT depends only on the palette, not targets.
    const cmb::CachedSearcher fs_searcher(fs, palette);
    const cmb::CachedSearcher pr_searcher(pr, palette);

    std::vector<MatchOutput> out;
    out.reserve(targets.size());
    for (const auto& t : targets) {
        // assemble fields: label is first, then t.fields
        std::vector<std::string> row;
        row.push_back(t.label);
        for (const auto& f : t.fields) row.push_back(f);

        std::string label;
        cmb::RGB8 rgb;
        if (!cmb::parse_color_row(row, fmt, rgb_scale, label, rgb)) continue;

        const cmb::Lab lab = cmb::rgb_to_lab(rgb);
        MatchOutput mo;
        mo.label = label;
        mo.target_rgb = rgb;
        mo.target_lab = lab;
        mo.fs = fs_searcher.match_one(lab, min_percent, exclude);
        mo.prusa = pr_searcher.match_one(lab, min_percent, exclude);
        out.push_back(std::move(mo));
    }
    return out;
}

// Parse exclude string "1-2,3-4" into pairs.
std::vector<std::pair<int,int>> parse_exclude(const std::string& s) {
    std::vector<std::pair<int,int>> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const size_t dash = tok.find('-');
        if (dash == std::string::npos) continue;
        try {
            out.emplace_back(std::stoi(tok.substr(0, dash)), std::stoi(tok.substr(dash + 1)));
        } catch (...) {}
    }
    return out;
}

// =========================================================================
// CLI arg parsing + usage (unchanged from before).
// =========================================================================
void print_usage()
{
    std::cerr <<
        "color_match_batch — FS vs Prusa color-mix reverse match\n"
        "\n"
        "USAGE (batch):\n"
        "  color_match_batch --palette P.csv --targets T.csv --output R.csv \\\n"
        "                    --input-format hex|rgb|lab|cmyk [options]\n"
        "USAGE (interactive, for web server):\n"
        "  color_match_batch --serve\n"
        "\n"
        "REQUIRED (batch):\n"
        "  --palette <file>        CSV of physical filament colors\n"
        "  --targets <file>        CSV of target colors (first column = label)\n"
        "  --output  <file>        Output CSV path\n"
        "  --input-format <fmt>    hex | rgb | lab | cmyk\n"
        "\n"
        "OPTIONS:\n"
        "  --min-percent <n>       Minimum per-component percent (0-50, default 0)\n"
        "  --exclude \"1-2,3-4\"     1-based palette-id pairs to skip\n"
        "  --rgb-scale auto|255|1  How to read numeric RGB columns\n"
        "  --serve                 Interactive stdin/stdout JSON mode\n"
        "  -h, --help              Show this message\n";
}

struct CliArgs {
    std::string palette_path;
    std::string targets_path;
    std::string output_path;
    cmb::ColorFormat fmt = cmb::ColorFormat::Hex;
    cmb::RgbScale rgb_scale = cmb::RgbScale::Auto;
    int min_percent = 0;
    std::string exclude_str;
    bool serve = false;
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
        else if (arg == "--serve") { out.serve = true; }
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
            out.exclude_str = v;
        }
        else {
            std::cerr << "error: unknown argument \"" << arg << "\"\n";
            return false;
        }
    }
    if (out.serve) { out.ok = true; return true; }
    if (out.palette_path.empty() || out.targets_path.empty() || out.output_path.empty()) {
        std::cerr << "error: --palette, --targets, --output are all required\n";
        return false;
    }
    out.ok = true;
    return true;
}

// =========================================================================
// CSV helpers (used by batch mode).
// =========================================================================
std::vector<std::vector<std::string>> read_csv_rows_file(const std::string& path) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(path);
    if (!in) return rows;
    std::string line;
    while (std::getline(in, line)) {
        bool only_ws = true;
        for (char c : line) if (c!=' '&&c!='\t'&&c!='\r'&&c!='\n') { only_ws=false; break; }
        if (only_ws) continue;
        rows.push_back(cmb::split_csv(line));
    }
    return rows;
}

bool load_palette_file(const std::string& path, std::vector<cmb::RGB8>& palette) {
    const auto rows = read_csv_rows_file(path);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& f = rows[i];
        if (f.empty()) continue;
        cmb::RGB8 rgb;
        bool parsed = cmb::parse_hex(f[0], rgb) || (f.size() > 1 && cmb::parse_hex(f[1], rgb));
        if (!parsed) { if (i == 0) continue; else continue; }
        palette.push_back(rgb);
    }
    return palette.size() >= 2;
}

// =========================================================================
// Output formatters.
// =========================================================================
void emit_color_csv(std::ostream& os, cmb::RGB8 c) {
    const cmb::Lab lab = cmb::rgb_to_lab(c);
    double C, M, Y, K;
    cmb::rgb_to_cmyk(c, C, M, Y, K);
    os << cmb::to_hex(c) << ","
       << (int)c.r << "," << (int)c.g << "," << (int)c.b << ","
       << cmb::fmt_double(lab.L) << "," << cmb::fmt_double(lab.a) << "," << cmb::fmt_double(lab.b) << ","
       << cmb::fmt_double(C * 100.0) << "," << cmb::fmt_double(M * 100.0) << ","
       << cmb::fmt_double(Y * 100.0) << "," << cmb::fmt_double(K * 100.0);
}

// Emit a recipe block to a JSON string (for serve mode).
// Keys are prefix-less (type, ids, hex, R, G, B, L, a, b, C, M, Y, K, delta_e)
// because each recipe sits inside its own "fs" / "prusa" object, so the prefix
// would be redundant. This matches what the browser expects (row.fs.delta_e).
std::string recipe_to_json(const cmb::MatchResult& r) {
    const char* type_str = !r.valid ? "none" : (r.type == cmb::MatchResult::Type::Pair ? "pair" : "triple");
    std::string ids, weights;
    for (size_t i = 0; i < r.ids.size(); ++i) { if (i) ids += "/"; ids += std::to_string(r.ids[i]); }
    for (size_t i = 0; i < r.weights.size(); ++i) { if (i) weights += "/"; weights += std::to_string(r.weights[i]); }

    const cmb::Lab lab = r.valid ? cmb::rgb_to_lab(r.preview_rgb) : cmb::Lab{0,0,0};
    double C = 0, M = 0, Y = 0, K = 0;
    if (r.valid) cmb::rgb_to_cmyk(r.preview_rgb, C, M, Y, K);

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "\"type\":\"%s\",\"ids\":\"%s\",\"weights\":\"%s\","
        "\"hex\":\"%s\",\"r\":%d,\"g\":%d,\"b\":%d,"
        "\"L\":%.4f,\"a\":%.4f,\"b_lab\":%.4f,"
        "\"C\":%.4f,\"M\":%.4f,\"Y\":%.4f,\"K\":%.4f,"
        "\"delta_e\":%.4f",
        type_str, ids.c_str(), weights.c_str(),
        r.valid ? cmb::to_hex(r.preview_rgb).c_str() : "", r.valid ? (int)r.preview_rgb.r : 0,
        r.valid ? (int)r.preview_rgb.g : 0, r.valid ? (int)r.preview_rgb.b : 0,
        lab.L, lab.a, lab.b,
        C, M, Y, K, r.delta_e);
    return buf;
}

std::string color_to_json(cmb::RGB8 c) {
    const cmb::Lab lab = cmb::rgb_to_lab(c);
    double C, M, Y, K;
    cmb::rgb_to_cmyk(c, C, M, Y, K);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "\"hex\":\"%s\",\"r\":%d,\"g\":%d,\"b\":%d,"
        "\"L\":%.4f,\"a\":%.4f,\"b_lab\":%.4f,"
        "\"C\":%.4f,\"M\":%.4f,\"Y\":%.4f,\"K\":%.4f",
        cmb::to_hex(c).c_str(), (int)c.r, (int)c.g, (int)c.b,
        lab.L, lab.a, lab.b, C, M, Y, K);
    return buf;
}

// =========================================================================
// Serve mode: read JSON requests from stdin, write JSON responses to stdout.
// =========================================================================
cmb::RgbScale parse_rgb_scale_str(const std::string& s) {
    if (s == "255") return cmb::RgbScale::Range255;
    if (s == "1")   return cmb::RgbScale::Range01;
    return cmb::RgbScale::Auto;
}

int run_serve_mode() {
    // Signal readiness so the Python wrapper knows we're alive.
    std::cout << "{\"ready\":true}\n" << std::flush;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string error_msg;
        try {
            JsonParser p(line);

            // palette: [["#hex","label"],...] → RGB8 vector
            const std::string pal_json = p.get_key("palette");
            std::vector<cmb::RGB8> palette;
            if (!pal_json.empty()) {
                JsonParser pp(pal_json);
                auto matrix = pp.parse_string_matrix(pal_json);
                for (const auto& row : matrix) {
                    cmb::RGB8 rgb;
                    bool parsed = false;
                    if (!row.empty() && cmb::parse_hex(row[0], rgb)) parsed = true;
                    else if (row.size() > 1 && cmb::parse_hex(row[1], rgb)) parsed = true;
                    if (parsed) palette.push_back(rgb);
                }
            }
            if (palette.size() < 2) {
                std::cout << "{\"error\":\"palette needs >= 2 colors\"}\n" << std::flush;
                continue;
            }

            // targets: [["label","comp",...],...]
            const std::string tgt_json = p.get_key("targets");
            std::vector<TargetInput> targets;
            if (!tgt_json.empty()) {
                JsonParser tp(tgt_json);
                auto matrix = tp.parse_string_matrix(tgt_json);
                for (const auto& row : matrix) {
                    if (row.empty()) continue;
                    TargetInput ti;
                    ti.label = row[0];
                    for (size_t i = 1; i < row.size(); ++i) ti.fields.push_back(row[i]);
                    targets.push_back(std::move(ti));
                }
            }
            if (targets.empty()) {
                std::cout << "{\"error\":\"no target rows\"}\n" << std::flush;
                continue;
            }

            // options
            std::string fmt_str = "hex";
            const std::string fmt_json = p.get_key("format");
            if (!fmt_json.empty()) {
                JsonParser fp(fmt_json);
                fmt_str = fp.parse_string_value();
            }
            cmb::ColorFormat fmt = cmb::ColorFormat::Hex;
            if (fmt_str == "rgb") fmt = cmb::ColorFormat::RGB;
            else if (fmt_str == "lab") fmt = cmb::ColorFormat::Lab;
            else if (fmt_str == "cmyk") fmt = cmb::ColorFormat::CMYK;

            cmb::RgbScale rgb_scale = cmb::RgbScale::Auto;
            const std::string rs_json = p.get_key("rgb_scale");
            if (!rs_json.empty()) {
                JsonParser rp(rs_json);
                rgb_scale = parse_rgb_scale_str(rp.parse_string_value());
            }

            int min_percent = 0;
            const std::string mp_json = p.get_key("min_percent");
            if (!mp_json.empty()) {
                JsonParser mp(mp_json);
                min_percent = std::atoi(mp.parse_literal().c_str());
            }

            std::string exclude_str;
            const std::string ex_json = p.get_key("exclude");
            if (!ex_json.empty()) {
                JsonParser ep(ex_json);
                exclude_str = ep.parse_string_value();
            }
            const auto exclude = parse_exclude(exclude_str);

            // run
            const auto results = run_match_for_targets(palette, targets, fmt, rgb_scale, min_percent, exclude);

            // build JSON response
            std::ostringstream os;
            os << "{\"rows\":[";
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& r = results[i];
                if (i) os << ",";
                os << "{\"label\":\"" << json_escape(r.label) << "\","
                   << "\"target\":{" << color_to_json(r.target_rgb) << "},"
                   << "\"fs\":{" << recipe_to_json(r.fs) << "},"
                   << "\"prusa\":{" << recipe_to_json(r.prusa) << "}}";
            }
            os << "],\"count\":" << results.size() << "}";
            std::cout << os.str() << "\n" << std::flush;
        } catch (const std::exception& e) {
            std::cout << "{\"error\":\"" << json_escape(e.what()) << "\"}\n" << std::flush;
        }
    }
    return 0;
}

// =========================================================================
// Batch CSV mode (original behavior, unchanged).
// =========================================================================
void emit_recipe_csv_header(std::ostream& os, const std::string& prefix) {
    os << prefix << "_type," << prefix << "_ids," << prefix << "_weights,"
       << prefix << "_hex," << prefix << "_R," << prefix << "_G," << prefix << "_B,"
       << prefix << "_L," << prefix << "_a," << prefix << "_b,"
       << prefix << "_C," << prefix << "_M," << prefix << "_Y," << prefix << "_K,"
       << prefix << "_delta_e";
}
void emit_recipe_csv_values(std::ostream& os, const cmb::MatchResult& r) {
    const char* type_str = !r.valid ? "none" : (r.type == cmb::MatchResult::Type::Pair ? "pair" : "triple");
    os << type_str << ",";
    std::string ids, weights;
    for (size_t i = 0; i < r.ids.size(); ++i) { if (i) ids += "/"; ids += std::to_string(r.ids[i]); }
    for (size_t i = 0; i < r.weights.size(); ++i) { if (i) weights += "/"; weights += std::to_string(r.weights[i]); }
    os << ids << "," << weights << ",";
    if (r.valid) {
        emit_color_csv(os, r.preview_rgb);
        os << "," << cmb::fmt_double(r.delta_e);
    } else {
        for (int i = 0; i < 12; ++i) { if (i) os << ","; }
    }
}

int run_batch_mode(const CliArgs& args) {
    std::vector<cmb::RGB8> palette;
    if (!load_palette_file(args.palette_path, palette)) {
        std::cerr << "error: palette needs at least 2 colors\n";
        return 1;
    }
    std::cerr << "palette: " << palette.size() << " colors loaded\n";

    const auto target_rows = read_csv_rows_file(args.targets_path);
    const auto exclude = parse_exclude(args.exclude_str);

    std::vector<TargetInput> targets;
    for (const auto& f : target_rows) {
        if (f.empty()) continue;
        TargetInput ti;
        ti.label = f[0];
        for (size_t i = 1; i < f.size(); ++i) ti.fields.push_back(f[i]);
        targets.push_back(std::move(ti));
    }

    const auto results = run_match_for_targets(
        palette, targets, args.fmt, args.rgb_scale, args.min_percent, exclude);

    std::ofstream out(args.output_path);
    if (!out) { std::cerr << "error: cannot write " << args.output_path << "\n"; return 1; }

    out << "label,";
    out << "target_hex,target_R,target_G,target_B,target_L,target_a,target_b,"
        << "target_C,target_M,target_Y,target_K,";
    emit_recipe_csv_header(out, "fs");     out << ",";
    emit_recipe_csv_header(out, "prusa");  out << "\n";

    size_t processed = 0;
    for (const auto& r : results) {
        out << r.label << ",";
        emit_color_csv(out, r.target_rgb);
        out << ",";
        emit_recipe_csv_values(out, r.fs);
        out << ",";
        emit_recipe_csv_values(out, r.prusa);
        out << "\n";
        ++processed;
    }
    std::cerr << "done: " << processed << " targets → " << args.output_path << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CliArgs args;
    if (!parse_args(argc, argv, args) || !args.ok) {
        return args.serve ? 1 : (args.palette_path.empty() && !args.serve ? 1 : 0);
    }
    if (args.serve) return run_serve_mode();
    return run_batch_mode(args);
}
