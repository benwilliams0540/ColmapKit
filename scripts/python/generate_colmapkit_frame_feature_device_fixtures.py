#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


WIDTH = 1024
HEIGHT = 768


def pixel_bytes(seed: int) -> bytes:
    pixels = bytearray(WIDTH * HEIGHT * 3)
    offset = 0
    for y in range(HEIGHT):
        for x in range(WIDTH):
            checker = 218 if ((x // (18 + seed) + y // (19 + seed)) % 2) else 28
            detail = (x * (13 + seed) + y * 29) % 47
            pixels[offset] = checker
            pixels[offset + 1] = (checker + detail) % 256
            pixels[offset + 2] = (255 - checker + detail) % 256
            offset += 3
    return bytes(pixels)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    entries = []
    for index, seed in enumerate((1, 2, 3), start=1):
        filename = f"frame-{index:02d}.jpg"
        output = args.output / filename
        with tempfile.TemporaryDirectory() as temporary:
            ppm = pathlib.Path(temporary) / f"frame-{index:02d}.ppm"
            ppm.write_bytes(
                f"P6\n{WIDTH} {HEIGHT}\n255\n".encode("ascii") + pixel_bytes(seed)
            )
            subprocess.run(
                [
                    "sips",
                    "-s",
                    "format",
                    "jpeg",
                    "-s",
                    "formatOptions",
                    "100",
                    str(ppm),
                    "--out",
                    str(output),
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
        encoded = output.read_bytes()
        entries.append(
            {
                "filename": filename,
                "sha256": hashlib.sha256(encoded).hexdigest(),
                "bytes": len(encoded),
                "width": WIDTH,
                "height": HEIGHT,
                "stableFrameID": 10 * index,
                "frameRevision": 1,
                "imageName": f"capture/{filename}",
                "seed": seed,
            }
        )

    manifest = {
        "schemaVersion": 1,
        "generator": "generate_colmapkit_frame_feature_device_fixtures.py",
        "encoder": "sips jpeg formatOptions 100",
        "cameraModel": "PINHOLE",
        "cameraParameters": [900.0, 900.0, WIDTH / 2, HEIGHT / 2],
        "frames": entries,
    }
    (args.output / "fixture-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
