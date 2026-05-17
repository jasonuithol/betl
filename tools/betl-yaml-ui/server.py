"""
Tiny FastAPI backend for betl-yaml-ui.

  pip install fastapi uvicorn
  python server.py [--root DIR] [--port N]

Then open http://127.0.0.1:8765/

Endpoints:
  GET /api/list?path=<rel>   list dirs + .yml/.yaml files under <root>/<rel>
  GET /api/file?path=<rel>   return raw YAML text
  PUT /api/file?path=<rel>   write raw YAML text (creates parents, overwrites)

All <rel> paths are resolved relative to --root and rejected if they escape it.
"""

import argparse
import sys
from pathlib import Path

try:
    from fastapi import FastAPI, HTTPException, Request
    from fastapi.responses import PlainTextResponse
    from fastapi.staticfiles import StaticFiles
    import uvicorn
except ImportError:
    sys.exit("missing deps. run: pip install fastapi uvicorn")


HERE = Path(__file__).resolve().parent
ALLOWED_EXTS = {".yml", ".yaml"}

app = FastAPI(title="betl-yaml-ui")
ROOT: Path = HERE  # set at startup


def safe(rel: str) -> Path:
    rel = (rel or "").replace("\\", "/").lstrip("/")
    p = (ROOT / rel).resolve()
    try:
        p.relative_to(ROOT)
    except ValueError:
        raise HTTPException(400, "path escapes root")
    return p


@app.get("/api/list")
def list_dir(path: str = ""):
    p = safe(path)
    if not p.exists():
        raise HTTPException(404, "not found")
    if not p.is_dir():
        raise HTTPException(400, "not a directory")
    dirs, files = [], []
    for entry in sorted(p.iterdir(), key=lambda e: (not e.is_dir(), e.name.lower())):
        if entry.name.startswith("."):
            continue
        rel = entry.relative_to(ROOT).as_posix()
        if entry.is_dir():
            dirs.append({"name": entry.name, "path": rel})
        elif entry.suffix.lower() in ALLOWED_EXTS:
            files.append({"name": entry.name, "path": rel, "size": entry.stat().st_size})
    return {
        "root": ROOT.as_posix(),
        "path": p.relative_to(ROOT).as_posix(),
        "dirs": dirs,
        "files": files,
    }


@app.get("/api/file")
def get_file(path: str):
    p = safe(path)
    if not p.exists() or not p.is_file():
        raise HTTPException(404, "not found")
    if p.suffix.lower() not in ALLOWED_EXTS:
        raise HTTPException(400, "only .yml/.yaml allowed")
    return PlainTextResponse(p.read_text(encoding="utf-8"))


@app.put("/api/file")
async def put_file(path: str, request: Request):
    p = safe(path)
    if p.suffix.lower() not in ALLOWED_EXTS:
        raise HTTPException(400, "only .yml/.yaml allowed")
    body = (await request.body()).decode("utf-8")
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(body, encoding="utf-8")
    return {
        "ok": True,
        "path": p.relative_to(ROOT).as_posix(),
        "size": len(body),
    }


# static viewer (registered last so /api/* routes win)
app.mount("/", StaticFiles(directory=HERE, html=True), name="ui")


def main():
    ap = argparse.ArgumentParser(description="betl-yaml-ui server")
    ap.add_argument(
        "--root",
        default=str(HERE.parent.parent),
        help="base directory the server may browse (default: repo root)",
    )
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()

    global ROOT
    ROOT = Path(args.root).resolve()
    if not ROOT.is_dir():
        sys.exit(f"--root is not a directory: {ROOT}")

    print(f"betl-yaml-ui  ui={HERE}  root={ROOT}  ->  http://{args.host}:{args.port}/")
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")


if __name__ == "__main__":
    main()
