#!/usr/bin/env python3
"""color-mixer-batch web UI.

A zero-dependency (stdlib only) local web server that wraps the compiled
color_match_batch CLI. The browser edits palette + target colors, this server
writes them to temp CSVs, spawns the C++ CLI, parses the result back into JSON,
and the page renders swatches + tables in real time.

Run:  python3 web/app.py   (or ./web/start.sh)
Then open http://localhost:8008

Requires build/color_match_batch[.exe] to already be built (run build.sh first).
"""
import csv
import io
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler
from urllib.parse import unquote

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WEB_DIR = HERE
STATIC_DIR = os.path.join(HERE, "static")
DEFAULT_PORT = 8008

# The built CLI binary, located relative to repo root.
def find_cli():
    for name in ("color_match_batch.exe", "color_match_batch"):
        p = os.path.join(ROOT, "build", name)
        if os.path.isfile(p):
            return p
    return None


# ---------------------------------------------------------------------------
# CSV helpers
# ---------------------------------------------------------------------------

def parse_csv_text(text):
    """Parse CSV text into a list of row-lists (strings), trimming blanks."""
    rows = []
    for raw in csv.reader(io.StringIO(text)):
        if not raw or all((c or "").strip() == "" for c in raw):
            continue
        rows.append([c.strip() for c in raw])
    return rows


def rows_to_csv_text(rows):
    out = io.StringIO()
    w = csv.writer(out)
    for r in rows:
        w.writerow(r)
    return out.getvalue()


# ---------------------------------------------------------------------------
# CLI invocation
# ---------------------------------------------------------------------------

def run_match(palette_rows, target_rows, fmt, opts):
    """Spawn the CLI with temp CSVs. Returns (rows, raw_csv, stderr, rc).

    palette_rows: list of [hex] or [hex, label] or [label, hex]
    target_rows:  list of [label, components...]
    fmt: "hex" | "rgb" | "lab" | "cmyk"
    opts: dict with optional keys: min_percent (int), exclude (str),
          rgb_scale ("auto"|"255"|"1")
    """
    cli = find_cli()
    if not cli:
        raise RuntimeError(
            "color_match_batch binary not found under build/. "
            "Run build.sh (or build.bat) first."
        )

    tmpdir = tempfile.mkdtemp(prefix="cmb_")
    try:
        pal_path = os.path.join(tmpdir, "palette.csv")
        tgt_path = os.path.join(tmpdir, "targets.csv")
        out_path = os.path.join(tmpdir, "results.csv")
        with open(pal_path, "w", newline="") as f:
            f.write(rows_to_csv_text(palette_rows))
        with open(tgt_path, "w", newline="") as f:
            f.write(rows_to_csv_text(target_rows))

        cmd = [cli, "--palette", pal_path, "--targets", tgt_path,
               "--output", out_path, "--input-format", fmt]
        if "min_percent" in opts and opts["min_percent"] is not None:
            cmd += ["--min-percent", str(opts["min_percent"])]
        if opts.get("exclude"):
            cmd += ["--exclude", str(opts["exclude"])]
        if opts.get("rgb_scale"):
            cmd += ["--rgb-scale", str(opts["rgb_scale"])]

        # Capture stderr (CLI logs progress there) but don't fail on it.
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        raw = ""
        if os.path.isfile(out_path):
            with open(out_path, "r", newline="") as f:
                raw = f.read()
        return raw, proc.stderr, proc.returncode
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def parse_results(raw_csv):
    """Parse the CLI's 42-column results CSV into a list of dict rows.

    Returns: list of {label, target:{hex,r,g,b,...}, fs:{...}, prusa:{...}}.
    """
    if not raw_csv.strip():
        return []
    reader = csv.DictReader(io.StringIO(raw_csv))
    rows = []
    for r in reader:
        def num(k, cast=float):
            v = r.get(k, "")
            try:
                return cast(v)
            except (ValueError, TypeError):
                return 0

        def block(prefix):
            return {
                "type": r.get(prefix + "_type", ""),
                "ids": r.get(prefix + "_ids", ""),
                "weights": r.get(prefix + "_weights", ""),
                "hex": r.get(prefix + "_hex", ""),
                "r": int(num(prefix + "_R", float) or 0),
                "g": int(num(prefix + "_G", float) or 0),
                "b": int(num(prefix + "_B", float) or 0),
                "L": num(prefix + "_L"),
                "a": num(prefix + "_a"),
                "b_lab": num(prefix + "_b"),
                "C": num(prefix + "_C"),
                "M": num(prefix + "_M"),
                "Y": num(prefix + "_Y"),
                "K": num(prefix + "_K"),
                "delta_e": num(prefix + "_delta_e"),
            }

        rows.append({
            "label": r.get("label", ""),
            "target": {
                "hex": r.get("target_hex", ""),
                "r": int(num("target_R", float) or 0),
                "g": int(num("target_G", float) or 0),
                "b": int(num("target_B", float) or 0),
                "L": num("target_L"), "a": num("target_a"),
                "b_lab": num("target_b"),
                "C": num("target_C"), "M": num("target_M"),
                "Y": num("target_Y"), "K": num("target_K"),
            },
            "fs": block("fs"),
            "prusa": block("prusa"),
        })
    return rows


# ---------------------------------------------------------------------------
# Default sample data (so the page isn't empty on first load)
# ---------------------------------------------------------------------------

DEFAULT_PALETTE = [
    ["#FF0000", "Red"], ["#00FF00", "Green"], ["#0000FF", "Blue"],
    ["#FFFF00", "Yellow"], ["#00FFFF", "Cyan"], ["#FF00FF", "Magenta"],
    ["#FFFFFF", "White"], ["#000000", "Black"],
]

DEFAULT_TARGETS_HEX = [
    ["orange", "#FF8800"], ["teal", "#008888"], ["purple", "#8800FF"],
    ["olive", "#666600"], ["gray", "#808080"], ["pink", "#FFAAAA"],
]

# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # silence default noisy logging

    def _send(self, code, body=b"", content_type="text/plain", extra_headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, obj, code=200):
        self._send(code, json.dumps(obj), "application/json")

    def do_GET(self):
        path = unquote(self.path.split("?", 1)[0])
        if path in ("/", "/index.html"):
            self._serve_static("index.html", "text/html; charset=utf-8")
        elif path == "/api/defaults":
            self._send_json({
                "palette": DEFAULT_PALETTE,
                "targets": DEFAULT_TARGETS_HEX,
                "format": "hex",
                "min_percent": 0,
                "exclude": "",
                "rgb_scale": "auto",
                "cli_found": find_cli() is not None,
            })
        else:
            # serve other static files (css/js) if added later
            rel = path.lstrip("/")
            candidate = os.path.join(STATIC_DIR, rel)
            if os.path.isfile(candidate):
                ctype = "application/octet-stream"
                if rel.endswith(".css"): ctype = "text/css"
                elif rel.endswith(".js"): ctype = "application/javascript"
                self._serve_file(candidate, ctype)
            else:
                self._send(404, "not found")

    def _serve_static(self, name, ctype):
        self._serve_file(os.path.join(STATIC_DIR, name), ctype)

    def _serve_file(self, path, ctype):
        try:
            with open(path, "rb") as f:
                body = f.read()
            self._send(200, body, ctype)
        except OSError:
            self._send(404, "not found")

    def do_POST(self):
        path = unquote(self.path)
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""

        if path == "/api/match":
            self._handle_match(raw, want_raw=False)
        elif path == "/api/export":
            self._handle_match(raw, want_raw=True)
        else:
            self._send(404, "not found")

    def _handle_match(self, raw_body, want_raw):
        try:
            payload = json.loads(raw_body.decode("utf-8") or "{}")
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            self._send_json({"error": "bad JSON: %s" % e}, 400)
            return

        palette = payload.get("palette") or DEFAULT_PALETTE
        targets = payload.get("targets") or []
        fmt = payload.get("format") or "hex"
        opts = {
            "min_percent": payload.get("min_percent"),
            "exclude": payload.get("exclude") or "",
            "rgb_scale": payload.get("rgb_scale") or "auto",
        }

        if len(targets) == 0:
            self._send_json({"error": "no target rows", "rows": []}, 400)
            return
        if fmt not in ("hex", "rgb", "lab", "cmyk"):
            self._send_json({"error": "bad format"}, 400)
            return

        try:
            raw_csv, stderr, rc = run_match(palette, targets, fmt, opts)
        except RuntimeError as e:
            self._send_json({"error": str(e)}, 500)
            return
        except subprocess.TimeoutExpired:
            self._send_json({"error": "CLI timed out (>60s)"}, 500)
            return
        except Exception as e:
            self._send_json({"error": "CLI failed: %s" % e}, 500)
            return

        if want_raw:
            # export endpoint: return the raw CSV for download
            fname = "results.csv"
            self._send(200, raw_csv, "text/csv",
                       extra_headers={"Content-Disposition": 'attachment; filename="%s"' % fname})
            return

        rows = parse_results(raw_csv)
        self._send_json({
            "rows": rows,
            "stderr": stderr,
            "rc": rc,
            "count": len(rows),
        })


def open_browser(url):
    """Best-effort cross-platform browser open, on a thread."""
    def _open():
        for cmd in (["start", "", url], ["xdg-open", url], ["open", url]):
            try:
                subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return
            except OSError:
                continue
    threading.Thread(target=_open, daemon=True).start()


def main():
    port = DEFAULT_PORT
    if find_cli() is None:
        print("[warn] build/color_match_batch not found — build it first (build.sh).", file=sys.stderr)
    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(("127.0.0.1", port), Handler)
    url = "http://localhost:%d" % port
    print("[color-mixer-batch] serving on %s" % url)
    print("  press Ctrl+C to stop")
    # Auto-open browser after a short delay.
    threading.Timer(0.5, lambda: open_browser(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[closed]")
        httpd.shutdown()


if __name__ == "__main__":
    main()
