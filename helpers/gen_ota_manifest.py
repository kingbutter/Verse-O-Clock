#!/usr/bin/env python3

"""
Generate OTA manifests for GitHub Release "latest/download" OTA.

Usage (from repo root):
  python helpers/gen_ota_manifest.py --device xiao_esp32c3_7p5 --dist dist --version v25.12.0

Creates:
  dist/<device>_ota.json

Expected in dist/:
  <device>_firmware.bin
  <device>_littlefs.bin   (optional)
"""
import argparse
import hashlib
import json
from pathlib import Path

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True)
    ap.add_argument("--dist", default="dist")
    ap.add_argument("--version", required=True)
    args = ap.parse_args()

    dist = Path(args.dist)
    fw = dist / f"{args.device}_firmware.bin"
    fs = dist / f"{args.device}_littlefs.bin"

    if not fw.exists():
        raise SystemExit(f"Missing firmware: {fw}")

    ver = args.version
    if not ver.startswith("v"):
        ver = "v" + ver

    manifest = {
        "device": args.device,
        "version": ver,
        "firmware": {"asset": fw.name, "sha256": sha256(fw), "size": fw.stat().st_size},
    }

    if fs.exists():
        manifest["littlefs"] = {"asset": fs.name, "sha256": sha256(fs), "size": fs.stat().st_size}

    out = dist / f"{args.device}_ota.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out}")

if __name__ == "__main__":
    main()
