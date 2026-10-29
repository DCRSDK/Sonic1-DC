#!/usr/bin/env python3
"""Reproduce/check the loose asset layout of the user's Sonic1test29.cdi.

The manifest contains names, hashes and audio metadata, never game content.
Stage WAVs are carried for reference parity; the current engine reads them
from Data.rsdk. Hashes document the reference, but are not enforced on freshly
encoded assets, whose bytes may vary with encoder versions.
"""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "stage-sfx"))
    parser.add_argument("data", type=Path)
    parser.add_argument("output", nargs="?", type=Path)
    args = parser.parse_args()
    manifest = json.loads(Path(__file__).with_name("reference-assets.json").read_text())
    assets = manifest["assets"]
    if args.action == "stage-sfx":
        if args.output is None:
            parser.error("stage-sfx requires source Data and output Data directories")
        for entry in assets:
            if "rate" not in entry:
                continue
            rel = Path(entry["path"]).relative_to("Data")
            source, target = args.data / rel, args.output / rel
            if not source.is_file():
                raise SystemExit(f"Missing reference source: {source}")
            target.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                            "-y", "-i", str(source), "-ar", str(entry["rate"]),
                            "-ac", str(entry["channels"]), "-c:a", "pcm_u8",
                            "-f", "wav", str(target)], check=True)
        return
    expected = {entry["path"][len("Data/"):] for entry in assets}
    actual = {p.relative_to(args.data).as_posix() for p in args.data.rglob("*") if p.is_file()}
    missing, extra = sorted(expected - actual), sorted(actual - expected)
    for path in missing:
        print(f"MISSING {path}")
    for path in extra:
        print(f"EXTRA {path}")
    print(f"Reference layout: {len(expected)} expected, {len(actual)} present; "
          f"{len(missing)} missing, {len(extra)} extra")
    raise SystemExit(bool(missing or extra))


if __name__ == "__main__":
    main()
