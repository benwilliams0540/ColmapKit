#!/usr/bin/env python3

"""Run a balanced, strict Standard-COLMAP CPU/Metal experiment."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import sqlite3
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


CANDIDATES = {
    "cpu": (False, False),
    "metal-sift": (True, False),
    "metal-matching": (False, True),
    "metal-combined": (True, True),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--colmapkit-bin", type=Path, required=True)
    parser.add_argument("--image-dir", type=Path, required=True)
    parser.add_argument("--image-list", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--max-image-size", type=int, default=1024)
    parser.add_argument("--sequential-overlap", type=int, default=4)
    parser.add_argument("--camera-model", default="SIMPLE_PINHOLE")
    parser.add_argument("--mapper-random-seed", type=int, default=0)
    parser.add_argument("--expected-manifest-sha256")
    parser.add_argument(
        "--reference-manifest-sha256",
        default="4433a3612c7c2e28c8a86e49c97e80178a586eac08ff1fe5ee0aa7a668aedf12",
        help="Historical downstream identity anchor; recorded, not recomputed.",
    )
    parser.add_argument(
        "--candidates",
        nargs="+",
        choices=tuple(CANDIDATES),
        default=tuple(CANDIDATES),
    )
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def selected_images(image_dir: Path, image_list: Path) -> list[Path]:
    names = [line.strip() for line in image_list.read_text().splitlines()]
    if not names or any(not name for name in names):
        raise SystemExit(f"Invalid or empty image list: {image_list}")
    paths = [image_dir / name for name in names]
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        raise SystemExit(f"Image list has missing files: {missing[:3]}")
    return paths


def image_manifest_sha256(image_dir: Path, image_list: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    paths = selected_images(image_dir, image_list)
    for path in paths:
        digest.update(
            f"{path.relative_to(image_dir)}\t{path.stat().st_size}\t"
            f"{sha256_file(path)}\n".encode()
        )
    return digest.hexdigest(), len(paths)


def command_output(command: list[str]) -> str:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    return (result.stdout + result.stderr).strip()


def host_snapshot() -> dict[str, str]:
    return {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "platform": platform.platform(),
        "power": command_output(["pmset", "-g", "batt"]),
        "thermal": command_output(["pmset", "-g", "therm"]),
    }


def sqlite_value_bytes(value: Any) -> bytes:
    if value is None:
        return b"N;"
    if isinstance(value, bytes):
        return b"B" + len(value).to_bytes(8, "big") + value
    return b"T" + str(value).encode("utf-8", errors="surrogateescape") + b";"


def database_summary(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {"present": False}
    connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        counts: dict[str, int] = {}
        logical = hashlib.sha256()
        tables = [
            row[0]
            for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"
            )
        ]
        for table in tables:
            quoted = '"' + table.replace('"', '""') + '"'
            counts[table] = connection.execute(
                f"SELECT count(*) FROM {quoted}"
            ).fetchone()[0]
            logical.update(table.encode() + b"\0")
            for row in connection.execute(f"SELECT * FROM {quoted} ORDER BY rowid"):
                for value in row:
                    logical.update(sqlite_value_bytes(value))
                logical.update(b"\n")
        return {
            "present": True,
            "byteSHA256": sha256_file(path),
            "logicalSHA256": logical.hexdigest(),
            "tableRowCounts": counts,
        }
    finally:
        connection.close()


def tree_hashes(root: Path) -> dict[str, str]:
    if not root.is_dir():
        return {}
    return {
        str(path.relative_to(root)): sha256_file(path)
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def verify_strict(label: str, evidence: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    metal_sift, metal_matching = CANDIDATES[label]
    metal = evidence.get("metal", {})
    strict = evidence.get("strict", {})
    effective = evidence.get("effective", {})
    if evidence.get("status") != 0:
        errors.append(f"evidence status is {evidence.get('status')}")
    if metal_sift:
        if not metal.get("siftActual") or metal.get("siftDispatchCount", 0) <= 0:
            errors.append("Metal SIFT did not prove dispatch")
        if metal.get("siftFallbackCount") != 0:
            errors.append("Metal SIFT reported fallback")
        if effective.get("featureExtractionBackend") != "metal":
            errors.append("feature extraction backend is not metal")
    elif metal.get("siftDispatchCount") != 0:
        errors.append("unexpected Metal SIFT dispatch")
    if metal_matching:
        if not metal.get("matchingActual") or metal.get("matchingDispatchCount", 0) <= 0:
            errors.append("Metal matching did not prove dispatch")
        if metal.get("matchingFallbackCount") != 0:
            errors.append("Metal matching reported fallback")
        if effective.get("featureMatchingBackend") != "metal":
            errors.append("feature matching backend is not metal")
    elif metal.get("matchingDispatchCount") != 0:
        errors.append("unexpected Metal matching dispatch")
    if (metal_sift or metal_matching) and not strict.get("noFallbackSatisfied"):
        errors.append("strict no-fallback contract was not satisfied")
    return errors


def run_candidate(
    args: argparse.Namespace, label: str, repeat: int, order: int
) -> dict[str, Any]:
    metal_sift, metal_matching = CANDIDATES[label]
    run_root = args.run_dir / "runs" / f"r{repeat + 1:02d}-o{order + 1:02d}-{label}"
    run_root.mkdir(parents=True)
    evidence_path = run_root / "evidence.json"
    log_path = run_root / "run.log"
    database_path = run_root / "database.db"
    sparse_path = run_root / "sparse"
    command = [
        str(args.colmapkit_bin),
        "--database_path", str(database_path),
        "--image_path", str(args.image_dir),
        "--output_path", str(sparse_path),
        "--image_list_path", str(args.image_list),
        "--camera_model", args.camera_model,
        "--max_image_size", str(args.max_image_size),
        "--matcher", "sequential",
        "--sequential_overlap", str(args.sequential_overlap),
        "--num_threads", str(args.workers),
        "--extraction_num_threads", str(args.workers),
        "--matching_num_threads", str(args.workers),
        "--mapper_num_threads", str(args.workers),
        "--mapper_random_seed", str(args.mapper_random_seed),
        "--use_metal_sift", str(int(metal_sift)),
        "--require_metal_sift", str(int(metal_sift)),
        "--use_metal_matching", str(int(metal_matching)),
        "--require_metal_matching", str(int(metal_matching)),
        "--evidence_path", str(evidence_path),
    ]
    start_snapshot = host_snapshot()
    start = time.perf_counter()
    with log_path.open("w", encoding="utf-8") as log:
        log.write("$ " + " ".join(command) + "\n")
        log.flush()
        process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    outer_seconds = time.perf_counter() - start
    end_snapshot = host_snapshot()
    evidence = json.loads(evidence_path.read_text()) if evidence_path.is_file() else {}
    strict_errors = verify_strict(label, evidence) if evidence else ["missing evidence"]
    return {
        "label": label,
        "repeat": repeat + 1,
        "orderWithinRepeat": order + 1,
        "processState": "new process; filesystem cache uncontrolled",
        "command": command,
        "returnCode": process.returncode,
        "outerWallSeconds": outer_seconds,
        "strictProofPassed": process.returncode == 0 and not strict_errors,
        "strictProofErrors": strict_errors,
        "hostStart": start_snapshot,
        "hostEnd": end_snapshot,
        "evidence": evidence,
        "database": database_summary(database_path),
        "sparseFileSHA256": tree_hashes(sparse_path),
        "log": str(log_path),
    }


def balanced_schedule(labels: list[str], repeats: int) -> list[list[str]]:
    return [
        labels[index % len(labels) :] + labels[: index % len(labels)]
        for index in range(repeats)
    ]


def write_csv(path: Path, runs: list[dict[str, Any]]) -> None:
    fields = [
        "label", "repeat", "orderWithinRepeat", "returnCode", "strictProofPassed",
        "outerWallSeconds", "featureExtraction", "featureMatching",
        "mappingBundleAdjustment", "export", "total", "residentPeakBytes",
        "registeredImages", "sparsePoints", "observations",
        "meanReprojectionError", "metalDevice", "metalSiftDispatches",
        "metalMatchingDispatches", "metalSiftFallbacks", "metalMatchingFallbacks",
    ]
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for run in runs:
            evidence = run.get("evidence", {})
            timings = evidence.get("timingsSeconds", {})
            resources = evidence.get("resources", {})
            output = evidence.get("output", {})
            metal = evidence.get("metal", {})
            writer.writerow({
                "label": run["label"],
                "repeat": run["repeat"],
                "orderWithinRepeat": run["orderWithinRepeat"],
                "returnCode": run["returnCode"],
                "strictProofPassed": run["strictProofPassed"],
                "outerWallSeconds": run["outerWallSeconds"],
                "featureExtraction": timings.get("featureExtraction"),
                "featureMatching": timings.get("featureMatching"),
                "mappingBundleAdjustment": timings.get("mappingBundleAdjustment"),
                "export": timings.get("export"),
                "total": timings.get("total"),
                "residentPeakBytes": resources.get("residentPeakBytes"),
                "registeredImages": output.get("registeredImages"),
                "sparsePoints": output.get("sparsePoints"),
                "observations": output.get("observations"),
                "meanReprojectionError": output.get("meanReprojectionError"),
                "metalDevice": metal.get("deviceName"),
                "metalSiftDispatches": metal.get("siftDispatchCount"),
                "metalMatchingDispatches": metal.get("matchingDispatchCount"),
                "metalSiftFallbacks": metal.get("siftFallbackCount"),
                "metalMatchingFallbacks": metal.get("matchingFallbackCount"),
            })


def main() -> int:
    args = parse_args()
    args.colmapkit_bin = args.colmapkit_bin.resolve()
    args.image_dir = args.image_dir.resolve()
    args.image_list = args.image_list.resolve()
    args.run_dir = args.run_dir.resolve()
    if args.repeats < 1:
        raise SystemExit("--repeats must be positive")
    if not args.colmapkit_bin.is_file() or not os.access(args.colmapkit_bin, os.X_OK):
        raise SystemExit(f"Missing executable: {args.colmapkit_bin}")
    if args.run_dir.exists() and any(args.run_dir.iterdir()):
        raise SystemExit(f"Refusing to overwrite non-empty run dir: {args.run_dir}")
    args.run_dir.mkdir(parents=True, exist_ok=True)

    manifest_sha, image_count = image_manifest_sha256(args.image_dir, args.image_list)
    if args.expected_manifest_sha256 and manifest_sha != args.expected_manifest_sha256:
        raise SystemExit(
            f"Manifest mismatch: expected {args.expected_manifest_sha256}, got {manifest_sha}"
        )
    source_commit = command_output(["git", "rev-parse", "HEAD"])
    source_diff = subprocess.run(
        ["git", "diff", "--binary", "HEAD"], capture_output=True, check=True
    ).stdout
    metadata = {
        "schemaVersion": 1,
        "sourceCommit": source_commit,
        "sourcePatchSHA256": hashlib.sha256(source_diff).hexdigest(),
        "binary": str(args.colmapkit_bin),
        "binarySHA256": sha256_file(args.colmapkit_bin),
        "imageDirectory": str(args.image_dir),
        "imageList": str(args.image_list),
        "imageCount": image_count,
        "selectedImageManifestSHA256": manifest_sha,
        "referenceManifestSHA256": args.reference_manifest_sha256,
        "configuration": {
            "cameraModel": args.camera_model,
            "matcher": "sequential",
            "maximumImageDimension": args.max_image_size,
            "sequentialOverlap": args.sequential_overlap,
            "workers": args.workers,
            "mapperRandomSeed": args.mapper_random_seed,
            "poseGPSOrientationFocusDepthPriors": False,
        },
        "schedule": balanced_schedule(list(args.candidates), args.repeats),
        "runs": [],
    }
    metadata_path = args.run_dir / "experiment.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    for repeat, labels in enumerate(metadata["schedule"]):
        for order, label in enumerate(labels):
            print(f"repeat {repeat + 1}/{args.repeats}, order {order + 1}: {label}", flush=True)
            run = run_candidate(args, label, repeat, order)
            metadata["runs"].append(run)
            metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
            if not run["strictProofPassed"]:
                print(
                    f"strict proof failed for {label}: {run['strictProofErrors']}",
                    file=sys.stderr,
                )

    write_csv(args.run_dir / "summary.csv", metadata["runs"])
    return 0 if all(run["strictProofPassed"] for run in metadata["runs"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
