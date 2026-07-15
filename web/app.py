#!/usr/bin/env python3
"""color-mixer-batch web UI — fast version.

Zero-dependency (stdlib only) local web server. On startup it spawns the
compiled CLI ONCE in --serve mode and keeps it alive, talking to it over
stdin/stdout pipes (one JSON request → one JSON response per line). This
avoids the ~50ms process-spawn cost per query that made the old version slow.

Run:  python3 web/app.py   (or ./web/start.sh)
Then open http://localhost:8008

Requires build/color_match_batch[.exe] to be built first (run build.sh).
"""
import atexit
import io
import json
import os
import socketserver
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler
from urllib.parse import unquote

# openpyxl is the only third-party dependency (for .xlsx read/write). Graceful
# degradation: the CSV/web-edit path works without it; only xlsx upload needs it.
try:
    import openpyxl
    from openpyxl.styles import PatternFill, Font, Alignment
    HAS_OPENPYXL = True
except ImportError:
    HAS_OPENPYXL = False

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
STATIC_DIR = os.path.join(HERE, "static")
DEFAULT_PORT = 8008


def find_cli():
    for name in ("color_match_batch.exe", "color_match_batch"):
        p = os.path.join(ROOT, "build", name)
        if os.path.isfile(p):
            return p
    return None


# ---------------------------------------------------------------------------
# Persistent CLI worker: one long-lived subprocess, JSON-over-pipes.
# ---------------------------------------------------------------------------

class CliWorker:
    """Manages the --serve subprocess. Thread-safe via a lock per call."""

    def __init__(self, cli_path):
        self.cli_path = cli_path
        self.proc = None
        self._lock = threading.Lock()
        self._start()

    def _start(self):
        self.proc = subprocess.Popen(
            [self.cli_path, "--serve"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=1, text=True, encoding="utf-8",
        )
        # Wait for the ready handshake.
        ready = self.proc.stdout.readline()
        if not ready.strip().startswith("{"):
            raise RuntimeError("CLI did not signal ready; got: %r" % ready)

    def query(self, request_dict):
        """Send one request, read one response. Returns parsed JSON dict."""
        line = json.dumps(request_dict, separators=(",", ":")) + "\n"
        with self._lock:
            if self.proc.poll() is not None:
                # process died — restart
                self._start()
            try:
                self.proc.stdin.write(line)
                self.proc.stdin.flush()
                resp = self.proc.stdout.readline()
            except (BrokenPipeError, OSError):
                self._start()
                self.proc.stdin.write(line)
                self.proc.stdin.flush()
                resp = self.proc.stdout.readline()
        if not resp:
            return {"error": "CLI returned empty response (crashed?)"}
        try:
            return json.loads(resp)
        except json.JSONDecodeError as e:
            return {"error": "bad JSON from CLI: %s — %r" % (e, resp[:200])}

    def shutdown(self):
        try:
            if self.proc and self.proc.poll() is None:
                self.proc.stdin.close()
                self.proc.wait(timeout=2)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


# Global worker — created lazily on first use so the server starts fast even
# if the CLI isn't ready yet (e.g. still building).
_WORKER = None
_WORKER_LOCK = threading.Lock()


def get_worker():
    global _WORKER
    if _WORKER is not None and _WORKER.proc and _WORKER.proc.poll() is None:
        return _WORKER
    with _WORKER_LOCK:
        if _WORKER is not None and _WORKER.proc and _WORKER.proc.poll() is None:
            return _WORKER
        cli = find_cli()
        if not cli:
            return None
        _WORKER = CliWorker(cli)
        return _WORKER


# ---------------------------------------------------------------------------
# Default sample data
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
# XLSX processing: read Lab targets from a sheet, batch-match, write results
# back into new sheets on the same workbook.
# ---------------------------------------------------------------------------

def _hex_fill(hex_str):
    """openpyxl PatternFill from a #rrggbb string (for colored result cells)."""
    h = hex_str.lstrip("#")
    if len(h) >= 6:
        return PatternFill(start_color="FF" + h[:6], end_color="FF" + h[:6], fill_type="solid")
    return None


def process_xlsx(xlsx_bytes, palette_rows, min_percent, exclude_str):
    """Read Lab targets from 'Sheet1', batch-match with the CLI worker, write
    FS results to 'FS_结果' and Prusa results to 'Prusa_结果' sheets.

    Returns the modified workbook as bytes (xlsx). Raises on error.
    Palette format: [["#hex","label"], ...] — same as the web UI uses.
    """
    if not HAS_OPENPYXL:
        raise RuntimeError("openpyxl not installed. Run: pip install openpyxl")
    if not palette_rows or len(palette_rows) < 2:
        raise RuntimeError("palette needs >= 2 colors")

    wb = openpyxl.load_workbook(io.BytesIO(xlsx_bytes), data_only=True)

    # ---- find the targets sheet: prefer "Sheet1", else first non-empty sheet ----
    targets_ws = None
    for name in ["Sheet1", "sheet1", "Targets", "目标色"]:
        if name in wb.sheetnames:
            targets_ws = wb[name]
            break
    if targets_ws is None:
        # pick the sheet with the most rows
        targets_ws = max(wb.worksheets, key=lambda ws: ws.max_row)
    if targets_ws.max_row < 2:
        raise RuntimeError("targets sheet '%s' appears empty" % targets_ws.title)

    # ---- parse palette rows → [hex or label, hex] ----
    palette = []
    for row in palette_rows:
        hex_val = None
        if row and row[0] and str(row[0]).startswith("#"):
            hex_val = row[0]
        elif len(row) > 1 and row[1] and str(row[1]).startswith("#"):
            hex_val = row[1]
        if hex_val:
            palette.append(hex_val)

    # ---- read targets (Lab format: 序号, L*, a*, b*) ----
    # Detect columns: find header row, then read L/a/b by header name.
    headers = {}
    for col in range(1, targets_ws.max_column + 1):
        val = targets_ws.cell(row=1, column=col).value
        if val is not None:
            headers[str(val).strip().lower()] = col

    # map common header variants
    def find_col(*names):
        for n in names:
            if n.lower() in headers: return headers[n.lower()]
        return None

    col_id = find_col("序号", "id", "no", "index", "#")
    col_L = find_col("l*", "l", "l*a*b*", "lightness")
    col_a = find_col("a*", "a")
    col_b = find_col("b*", "b")

    if col_L is None or col_a is None or col_b is None:
        # fallback: assume columns B/C/D = L/a/b (A = id)
        col_id, col_L, col_a, col_b = 1, 2, 3, 4

    # collect target rows: {label, L, a, b}
    targets = []
    for r in range(2, targets_ws.max_row + 1):
        L = targets_ws.cell(row=r, column=col_L).value
        a = targets_ws.cell(row=r, column=col_a).value
        b = targets_ws.cell(row=r, column=col_b).value
        label = targets_ws.cell(row=r, column=col_id).value if col_id else r - 1
        if L is None or a is None or b is None:
            continue
        try:
            targets.append({"label": str(label), "L": float(L), "a": float(a), "b": float(b)})
        except (ValueError, TypeError):
            continue

    if not targets:
        raise RuntimeError("no valid Lab target rows found in '%s'" % targets_ws.title)

    # ---- run batch match via the persistent CLI worker ----
    worker = get_worker()
    if worker is None:
        raise RuntimeError("CLI worker not available")

    # CLI expects targets as [["label", L, a, b], ...] in "lab" format
    target_rows = [[t["label"], str(t["L"]), str(t["a"]), str(t["b"])] for t in targets]
    request = {
        "palette": palette_rows,
        "targets": target_rows,
        "format": "lab",
        "rgb_scale": "auto",
        "min_percent": min_percent if min_percent is not None else 0,
        "exclude": exclude_str or "",
    }
    result = worker.query(request)
    if "error" in result and "rows" not in result:
        raise RuntimeError("CLI error: %s" % result["error"])

    rows = result.get("rows", [])

    # ---- write results into two new sheets ----
    RESULT_COLUMNS = [
        ("序号", "label"),
        ("目标Lab_L", None), ("目标Lab_a", None), ("目标Lab_b", None),
        ("目标_RGB", "target_hex"),
        ("配方类型", "type"), ("耗材ID", "ids"), ("比例", "weights"),
        ("预测_RGB", "hex"), ("预测_L", "L"), ("预测_a", "a"), ("预测_b", "b_lab"),
        ("ΔE2000", "delta_e"),
    ]

    for sheet_name, prefix in [("FS_结果", "fs"), ("Prusa_结果", "prusa")]:
        # remove old sheet if re-running
        if sheet_name in wb.sheetnames:
            del wb[sheet_name]
        ws = wb.create_sheet(sheet_name)

        # header row (bold)
        header_font = Font(bold=True)
        for c, (title, _) in enumerate(RESULT_COLUMNS, 1):
            cell = ws.cell(row=1, column=c, value=title)
            cell.font = header_font

        for r_idx, row in enumerate(rows, 2):
            block = row[prefix]
            target = row["target"]
            for c, (title, key) in enumerate(RESULT_COLUMNS, 1):
                if title == "序号":
                    val = row.get("label", "")
                elif title == "目标Lab_L":
                    val = next((t["L"] for t in targets if t["label"] == row["label"]), "")
                elif title == "目标Lab_a":
                    val = next((t["a"] for t in targets if t["label"] == row["label"]), "")
                elif title == "目标Lab_b":
                    val = next((t["b"] for t in targets if t["label"] == row["label"]), "")
                elif title == "目标_RGB":
                    val = target.get("hex", "")
                elif key:
                    val = block.get(key, "")
                else:
                    val = ""
                cell = ws.cell(row=r_idx, column=c, value=val)

            # color-fill the RGB cells (target + predicted)
            target_hex = target.get("hex", "")
            if target_hex:
                ws.cell(row=r_idx, column=5).fill = _hex_fill(target_hex)
            pred_hex = block.get("hex", "")
            if pred_hex:
                ws.cell(row=r_idx, column=9).fill = _hex_fill(pred_hex)

        # auto column width (rough)
        for c in range(1, len(RESULT_COLUMNS) + 1):
            ws.column_dimensions[openpyxl.utils.get_column_letter(c)].width = 14

    # ---- serialize workbook to bytes ----
    out = io.BytesIO()
    wb.save(out)
    return out.getvalue(), len(rows)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

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
            self._serve_file(os.path.join(STATIC_DIR, "index.html"), "text/html; charset=utf-8")
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
            rel = path.lstrip("/")
            candidate = os.path.join(STATIC_DIR, rel)
            if os.path.isfile(candidate):
                ctype = {"css": "text/css", "js": "application/javascript"}.get(
                    rel.rsplit(".", 1)[-1], "application/octet-stream")
                self._serve_file(candidate, ctype)
            else:
                self._send(404, "not found")

    def _serve_file(self, path, ctype):
        try:
            with open(path, "rb") as f:
                self._send(200, f.read(), ctype)
        except OSError:
            self._send(404, "not found")

    def do_POST(self):
        path = unquote(self.path)
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        if path == "/api/match":
            self._handle_match(raw)
        elif path == "/api/export":
            self._handle_export(raw)
        elif path == "/api/xlsx/run":
            self._handle_xlsx(raw)
        else:
            self._send(404, "not found")

    def _handle_match(self, raw_body):
        try:
            payload = json.loads(raw_body.decode("utf-8") or "{}")
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            self._send_json({"error": "bad JSON: %s" % e}, 400)
            return

        request = {
            "palette": payload.get("palette") or DEFAULT_PALETTE,
            "targets": payload.get("targets") or [],
            "format": payload.get("format") or "hex",
            "rgb_scale": payload.get("rgb_scale") or "auto",
            "min_percent": payload.get("min_percent") if payload.get("min_percent") is not None else 0,
            "exclude": payload.get("exclude") or "",
        }
        if len(request["targets"]) == 0:
            self._send_json({"error": "no target rows", "rows": []}, 400)
            return

        worker = get_worker()
        if worker is None:
            self._send_json({"error": "CLI binary not found under build/. Run build.sh first."}, 500)
            return

        result = worker.query(request)
        if "error" in result and "rows" not in result:
            self._send_json(result, 400)
        else:
            self._send_json(result)

    def _handle_export(self, raw_body):
        """Export uses CSV columns. We run the same query and reformat to CSV
        client-side in the browser, so here we just return the JSON match
        result with a CSV content-type hint. Simpler: return JSON and let JS
        build CSV. But to keep one code path, we reuse /api/match logic."""
        # The browser's export already calls /api/match and builds CSV in JS
        # (see index.html exportCsv). This endpoint is kept for compatibility
        # but delegates to the same worker.
        self._handle_match(raw_body)

    def _handle_xlsx(self, raw_body):
        """Upload a .xlsx, batch-match its Lab targets with the current palette,
        write FS/Prusa results into new sheets, return the modified .xlsx.

        Request is multipart/form-data OR JSON with base64 file content.
        We accept JSON for simplicity: {xlsx: "<base64>", palette: [...],
        min_percent: N, exclude: "..."}.
        """
        try:
            import base64
            payload = json.loads(raw_body.decode("utf-8") or "{}")
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            self._send_json({"error": "bad request: %s" % e}, 400)
            return

        xlsx_b64 = payload.get("xlsx")
        if not xlsx_b64:
            self._send_json({"error": "missing 'xlsx' field (base64-encoded file)"}, 400)
            return
        try:
            xlsx_bytes = base64.b64decode(xlsx_b64)
        except Exception as e:
            self._send_json({"error": "bad base64: %s" % e}, 400)
            return

        palette = payload.get("palette") or DEFAULT_PALETTE
        min_percent = payload.get("min_percent")
        exclude = payload.get("exclude") or ""

        try:
            data, count = process_xlsx(xlsx_bytes, palette, min_percent, exclude)
        except Exception as e:
            self._send_json({"error": str(e)}, 500)
            return

        # Return the xlsx bytes directly for download.
        self._send(200, data, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                   extra_headers={"Content-Disposition": 'attachment; filename="results.xlsx"'})


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def open_browser_async(url):
    def _open():
        for cmd in (["start", "", url], ["xdg-open", url], ["open", url]):
            try:
                subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return
            except OSError:
                continue
    threading.Thread(target=_open, daemon=True).start()


def _port_in_use(port):
    """Quick check whether a TCP port is already bound."""
    import socket as _sock
    s = _sock.socket(_sock.AF_INET, _sock.SOCK_STREAM)
    s.settimeout(0.3)
    try:
        s.bind(("127.0.0.1", port))
        s.close()
        return False
    except OSError:
        return True


def main():
    cli = find_cli()
    if cli is None:
        print("[warn] build/color_match_batch not found — build it first (build.sh).", file=sys.stderr)

    # Pre-warm the CLI worker NOW (not on first request) so the first match
    # is just as fast as subsequent ones.
    if cli:
        try:
            get_worker()
            print("[ok] CLI worker ready")
        except Exception as e:
            print("[warn] CLI worker failed to start: %s" % e, file=sys.stderr)

    # Find a free port: try DEFAULT_PORT, then increment up to +20.
    socketserver.TCPServer.allow_reuse_address = True
    port = DEFAULT_PORT
    httpd = None
    for attempt in range(20):
        if _port_in_use(port):
            print("[info] port %d busy, trying %d ..." % (port, port + 1))
            port += 1
            continue
        try:
            httpd = socketserver.TCPServer(("127.0.0.1", port), Handler)
            break
        except OSError:
            port += 1
    if httpd is None:
        print("[error] could not find a free port in %d-%d" % (DEFAULT_PORT, port), file=sys.stderr)
        sys.exit(1)

    def _cleanup():
        global _WORKER
        if _WORKER:
            _WORKER.shutdown()
        httpd.shutdown()
    atexit.register(_cleanup)

    url = "http://localhost:%d" % port
    print("[color-mixer-batch] serving on %s" % url)
    print("  (Ctrl+C to stop)")
    # Flush so the user sees the URL before the browser opens.
    sys.stdout.flush()
    threading.Timer(0.5, lambda: open_browser_async(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[closed]")


if __name__ == "__main__":
    main()
