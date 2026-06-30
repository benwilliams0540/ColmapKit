#!/usr/bin/env python3

import argparse
import csv
import json
import re
import shutil
import sqlite3
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


CAPACITY_DROP_RE = re.compile(
    r"(?P<stage>[A-Za-z0-9_.-]+): .*?observed=(?P<observed>[0-9]+), "
    r"capacity=(?P<capacity>[0-9]+), dropped=(?P<dropped>[0-9]+)"
)

LIMIT_DROP_RE = re.compile(
    r"(?P<stage>[A-Za-z0-9_.-]+): .*?observed=(?P<observed>[0-9]+), "
    r"(?P<limit_name>max_num_[A-Za-z0-9_]+)=(?P<limit>[0-9]+), "
    r"dropped=(?P<dropped>[0-9]+)"
)

MODEL_ANALYZER_PATTERNS = {
    "registered_images": re.compile(r"Registered images:\s+([0-9]+)"),
    "sparse_points": re.compile(r"Points:\s+([0-9]+)"),
    "observations": re.compile(r"Observations:\s+([0-9]+)"),
    "mean_reprojection_error_px": re.compile(
        r"Mean reprojection error:\s+([0-9.eE+-]+)px"
    ),
}


@dataclass(frozen=True)
class ExtractionRun:
    label: str
    backend: str
    colmap_bin: Path
    db_path: Path
    log_path: Path
    args: list[str]


@dataclass(frozen=True)
class PipelineRun:
    label: str
    extractor_label: str
    matcher_backend: str
    colmap_bin: Path
    db_path: Path
    sparse_dir: Path
    match_log_path: Path
    mapper_log_path: Path
    match_args: list[str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare COLMAP CPU, optional CUDA/SiftGPU, and Metal SIFT "
            "extraction with exact-cap diagnostics and sparse model metrics."
        )
    )
    parser.add_argument("--colmap-bin", type=Path, required=True)
    parser.add_argument("--image-dir", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--image-list-path", type=Path)
    parser.add_argument("--single-cpu-threads", type=int, default=1)
    parser.add_argument("--threaded-cpu-threads", type=int, default=6)
    parser.add_argument("--metal-threads", type=int, default=1)
    parser.add_argument("--mapper-threads", type=int, default=6)
    parser.add_argument("--matcher-threads", type=int, default=1)
    parser.add_argument("--max-image-size", type=int, default=1000)
    parser.add_argument("--max-num-features", type=int, default=1024)
    parser.add_argument("--max-num-orientations", type=int, default=2)
    parser.add_argument("--overlap", type=int, default=10)
    parser.add_argument(
        "--exact-feature-cap",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Post-cap keypoint and descriptor rows to max-num-features.",
    )
    parser.add_argument(
        "--include-cuda",
        action="store_true",
        help=(
            "Also run a GPU SIFT extractor path. This requires a COLMAP binary "
            "configured with CUDA/OpenGL SiftGPU support."
        ),
    )
    parser.add_argument("--cuda-colmap-bin", type=Path)
    parser.add_argument("--cuda-threads", type=int, default=1)
    parser.add_argument("--cuda-gpu-index", default="0")
    return parser.parse_args()


def check_input_paths(args: argparse.Namespace) -> None:
    if not args.colmap_bin.exists():
        raise SystemExit(f"Missing COLMAP binary: {args.colmap_bin}")
    if not args.image_dir.is_dir():
        raise SystemExit(f"Missing image directory: {args.image_dir}")
    if args.image_list_path is not None and not args.image_list_path.is_file():
        raise SystemExit(f"Missing image list: {args.image_list_path}")
    if args.cuda_colmap_bin is not None and not args.cuda_colmap_bin.exists():
        raise SystemExit(f"Missing CUDA COLMAP binary: {args.cuda_colmap_bin}")
    if args.run_dir.exists() and any(args.run_dir.iterdir()):
        raise SystemExit(f"Refusing to overwrite non-empty run dir: {args.run_dir}")


def run_command(label: str, command: list[str], log_path: Path) -> float:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"\n== {label} ==")
    print(" ".join(command))

    start = time.perf_counter()
    with log_path.open("w", encoding="utf-8") as log_file:
        log_file.write("$ " + " ".join(command) + "\n")
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="")
            log_file.write(line)
        return_code = process.wait()

    elapsed_seconds = time.perf_counter() - start
    print(f"{label}: {elapsed_seconds:.2f}s")
    if return_code != 0:
        raise subprocess.CalledProcessError(return_code, command)
    return elapsed_seconds


def common_feature_args(args: argparse.Namespace) -> list[str]:
    feature_args = [
        "--image_path",
        str(args.image_dir),
        "--ImageReader.single_camera",
        "1",
        "--FeatureExtraction.max_image_size",
        str(args.max_image_size),
        "--SiftExtraction.max_num_features",
        str(args.max_num_features),
        "--SiftExtraction.max_num_orientations",
        str(args.max_num_orientations),
    ]
    if args.image_list_path is not None:
        feature_args += ["--image_list_path", str(args.image_list_path)]
    return feature_args


def build_extraction_runs(args: argparse.Namespace) -> list[ExtractionRun]:
    db_dir = args.run_dir / "databases"
    log_dir = args.run_dir / "logs"
    common_args = common_feature_args(args)
    runs = [
        ExtractionRun(
            label="cpu_single",
            backend="cpu",
            colmap_bin=args.colmap_bin,
            db_path=db_dir / "cpu_single.db",
            log_path=log_dir / "extract_cpu_single.log",
            args=[
                *common_args,
                "--FeatureExtraction.use_gpu",
                "0",
                "--SiftExtraction.use_metal",
                "0",
                "--FeatureExtraction.num_threads",
                str(args.single_cpu_threads),
            ],
        ),
        ExtractionRun(
            label="cpu_threaded",
            backend="cpu",
            colmap_bin=args.colmap_bin,
            db_path=db_dir / "cpu_threaded.db",
            log_path=log_dir / "extract_cpu_threaded.log",
            args=[
                *common_args,
                "--FeatureExtraction.use_gpu",
                "0",
                "--SiftExtraction.use_metal",
                "0",
                "--FeatureExtraction.num_threads",
                str(args.threaded_cpu_threads),
            ],
        ),
    ]

    if args.include_cuda:
        runs.append(
            ExtractionRun(
                label="cuda_gpu",
                backend="cuda_or_siftgpu",
                colmap_bin=args.cuda_colmap_bin or args.colmap_bin,
                db_path=db_dir / "cuda_gpu.db",
                log_path=log_dir / "extract_cuda_gpu.log",
                args=[
                    *common_args,
                    "--FeatureExtraction.use_gpu",
                    "1",
                    "--SiftExtraction.use_metal",
                    "0",
                    "--FeatureExtraction.gpu_index",
                    args.cuda_gpu_index,
                    "--FeatureExtraction.num_threads",
                    str(args.cuda_threads),
                ],
            )
        )

    runs.append(
        ExtractionRun(
            label="metal_extractor",
            backend="metal",
            colmap_bin=args.colmap_bin,
            db_path=db_dir / "metal_extractor.db",
            log_path=log_dir / "extract_metal.log",
            args=[
                *common_args,
                "--FeatureExtraction.use_gpu",
                "1",
                "--SiftExtraction.use_metal",
                "1",
                "--FeatureExtraction.num_threads",
                str(args.metal_threads),
            ],
        )
    )
    return runs


def summarize_database(db_path: Path) -> dict[str, int]:
    con = sqlite3.connect(db_path)
    cur = con.cursor()
    summary = {
        "images": cur.execute("select count(*) from images").fetchone()[0],
        "keypoints": cur.execute(
            "select coalesce(sum(rows), 0) from keypoints"
        ).fetchone()[0],
        "descriptors": cur.execute(
            "select coalesce(sum(rows), 0) from descriptors"
        ).fetchone()[0],
        "raw_matches": cur.execute(
            "select coalesce(sum(rows), 0) from matches where rows > 0"
        ).fetchone()[0],
        "raw_pairs": cur.execute(
            "select count(*) from matches where rows > 0"
        ).fetchone()[0],
        "verified_inliers": cur.execute(
            "select coalesce(sum(rows), 0) from two_view_geometries "
            "where rows > 0"
        ).fetchone()[0],
        "verified_pairs": cur.execute(
            "select count(*) from two_view_geometries where rows > 0"
        ).fetchone()[0],
    }
    con.close()
    return summary


def cap_table_rows(cur: sqlite3.Cursor, table: str, row_cap: int) -> dict[str, int]:
    rows = cur.execute(
        f"select image_id, rows, data from {table} where rows > ?",
        (row_cap,),
    ).fetchall()
    dropped_rows = 0
    updated_images = 0
    max_rows_before = 0
    for image_id, num_rows, data in rows:
        max_rows_before = max(max_rows_before, num_rows)
        dropped_rows += num_rows - row_cap
        if data is None or num_rows <= 0:
            continue
        if len(data) % num_rows != 0:
            raise ValueError(
                f"{table} image_id={image_id} has non-integral row stride"
            )
        bytes_per_row = len(data) // num_rows
        cur.execute(
            f"update {table} set rows = ?, data = ? where image_id = ?",
            (row_cap, data[: row_cap * bytes_per_row], image_id),
        )
        updated_images += 1
    return {
        "updated_images": updated_images,
        "dropped_rows": dropped_rows,
        "max_rows_before": max_rows_before,
    }


def apply_exact_feature_cap(db_path: Path, row_cap: int) -> dict[str, int]:
    before = summarize_database(db_path)
    con = sqlite3.connect(db_path)
    cur = con.cursor()
    keypoints = cap_table_rows(cur, "keypoints", row_cap)
    descriptors = cap_table_rows(cur, "descriptors", row_cap)
    con.commit()
    con.close()
    after = summarize_database(db_path)
    return {
        "row_cap": row_cap,
        "keypoints_before": before["keypoints"],
        "keypoints_after": after["keypoints"],
        "descriptors_before": before["descriptors"],
        "descriptors_after": after["descriptors"],
        "keypoint_images_capped": keypoints["updated_images"],
        "descriptor_images_capped": descriptors["updated_images"],
        "keypoint_rows_dropped": keypoints["dropped_rows"],
        "descriptor_rows_dropped": descriptors["dropped_rows"],
        "max_keypoint_rows_before": keypoints["max_rows_before"],
        "max_descriptor_rows_before": descriptors["max_rows_before"],
    }


def add_drop_summary(
    stages: dict[str, dict[str, int]],
    stage: str,
    observed: int,
    limit: int,
    dropped: int,
    limit_key: str,
) -> None:
    stage_summary = stages.setdefault(
        stage,
        {
            "events": 0,
            "observed_total": 0,
            f"{limit_key}_min": limit,
            f"{limit_key}_max": limit,
            "dropped_total": 0,
            "max_dropped": 0,
        },
    )
    stage_summary["events"] += 1
    stage_summary["observed_total"] += observed
    stage_summary[f"{limit_key}_min"] = min(
        stage_summary[f"{limit_key}_min"], limit
    )
    stage_summary[f"{limit_key}_max"] = max(
        stage_summary[f"{limit_key}_max"], limit
    )
    stage_summary["dropped_total"] += dropped
    stage_summary["max_dropped"] = max(stage_summary["max_dropped"], dropped)


def parse_capacity_drops(log_path: Path) -> dict[str, Any]:
    capacity_stages: dict[str, dict[str, int]] = {}
    limit_stages: dict[str, dict[str, int]] = {}
    fallback_lines: list[str] = []
    with log_path.open("r", encoding="utf-8", errors="replace") as log_file:
        for line in log_file:
            for match in CAPACITY_DROP_RE.finditer(line):
                add_drop_summary(
                    capacity_stages,
                    match.group("stage"),
                    int(match.group("observed")),
                    int(match.group("capacity")),
                    int(match.group("dropped")),
                    "capacity",
                )
            for match in LIMIT_DROP_RE.finditer(line):
                add_drop_summary(
                    limit_stages,
                    match.group("stage"),
                    int(match.group("observed")),
                    int(match.group("limit")),
                    int(match.group("dropped")),
                    "limit",
                )
            lower_line = line.lower()
            if (
                "fallback" in lower_line
                or "falling back" in lower_line
                or "unavailable" in lower_line
            ):
                fallback_lines.append(line.strip())
    total_capacity_dropped = sum(
        stage["dropped_total"] for stage in capacity_stages.values()
    )
    total_limit_dropped = sum(
        stage["dropped_total"] for stage in limit_stages.values()
    )
    return {
        "stages": capacity_stages,
        "capacity_stages": capacity_stages,
        "limit_stages": limit_stages,
        "total_capacity_dropped": total_capacity_dropped,
        "total_limit_dropped": total_limit_dropped,
        "total_dropped": total_capacity_dropped + total_limit_dropped,
        "fallback_lines": fallback_lines[:20],
    }


def cpu_match_args(args: argparse.Namespace) -> list[str]:
    return [
        "--FeatureMatching.use_gpu",
        "0",
        "--SiftMatching.cpu_brute_force_matcher",
        "1",
        "--SiftMatching.use_metal",
        "0",
        "--SequentialMatching.overlap",
        str(args.overlap),
        "--SequentialMatching.loop_detection",
        "0",
        "--FeatureMatching.num_threads",
        str(args.matcher_threads),
    ]


def metal_match_args(args: argparse.Namespace) -> list[str]:
    return [
        "--FeatureMatching.use_gpu",
        "1",
        "--SiftMatching.use_metal",
        "1",
        "--SequentialMatching.overlap",
        str(args.overlap),
        "--SequentialMatching.loop_detection",
        "0",
        "--FeatureMatching.num_threads",
        str(args.matcher_threads),
    ]


def build_pipeline_runs(
    args: argparse.Namespace, extraction_runs: list[ExtractionRun]
) -> list[PipelineRun]:
    log_dir = args.run_dir / "logs"
    sparse_dir = args.run_dir / "sparse"
    pipelines = [
        PipelineRun(
            label=run.label,
            extractor_label=run.label,
            matcher_backend="cpu",
            colmap_bin=run.colmap_bin,
            db_path=run.db_path,
            sparse_dir=sparse_dir / run.label,
            match_log_path=log_dir / f"match_{run.label}.log",
            mapper_log_path=log_dir / f"mapper_{run.label}.log",
            match_args=cpu_match_args(args),
        )
        for run in extraction_runs
    ]

    metal_run = next(run for run in extraction_runs if run.label == "metal_extractor")
    metal_full_db = args.run_dir / "databases" / "metal_full.db"
    shutil.copy2(metal_run.db_path, metal_full_db)
    pipelines.append(
        PipelineRun(
            label="metal_full",
            extractor_label=metal_run.label,
            matcher_backend="metal",
            colmap_bin=metal_run.colmap_bin,
            db_path=metal_full_db,
            sparse_dir=sparse_dir / "metal_full",
            match_log_path=log_dir / "match_metal_full.log",
            mapper_log_path=log_dir / "mapper_metal_full.log",
            match_args=metal_match_args(args),
        )
    )
    return pipelines


def parse_model_analyzer(log_path: Path) -> dict[str, int | float | None]:
    text = log_path.read_text(encoding="utf-8", errors="replace")
    metrics: dict[str, int | float | None] = {}
    for key, pattern in MODEL_ANALYZER_PATTERNS.items():
        match = pattern.search(text)
        if match is None:
            metrics[key] = None
        elif key == "mean_reprojection_error_px":
            metrics[key] = float(match.group(1))
        else:
            metrics[key] = int(match.group(1))
    return metrics


def sort_model_dirs(model_dirs: list[Path]) -> list[Path]:
    def key(path: Path) -> tuple[int, int | str]:
        if path.name.isdigit():
            return (0, int(path.name))
        return (1, path.name)

    return sorted(model_dirs, key=key)


def analyze_sparse_models(
    colmap_bin: Path, sparse_dir: Path, log_dir: Path, label: str
) -> dict[str, Any]:
    model_dirs = sort_model_dirs([path for path in sparse_dir.iterdir() if path.is_dir()])
    models = []
    for model_dir in model_dirs:
        log_path = log_dir / f"model_analyzer_{label}_{model_dir.name}.log"
        elapsed_seconds = run_command(
            f"model_analyzer:{label}:{model_dir.name}",
            [str(colmap_bin), "model_analyzer", "--path", str(model_dir)],
            log_path,
        )
        metrics = parse_model_analyzer(log_path)
        metrics.update(
            {
                "model": model_dir.name,
                "path": str(model_dir),
                "analyzer_seconds": elapsed_seconds,
            }
        )
        models.append(metrics)

    if not models:
        return {
            "model_count": 0,
            "registered_images_by_model": [],
            "largest_model": None,
            "models": [],
        }

    largest_model = max(
        models,
        key=lambda model: (
            model.get("registered_images") or 0,
            model.get("sparse_points") or 0,
            model.get("observations") or 0,
        ),
    )
    return {
        "model_count": len(models),
        "registered_images_by_model": [
            model.get("registered_images") for model in models
        ],
        "largest_model": largest_model,
        "models": models,
    }


def run_extractions(args: argparse.Namespace) -> dict[str, dict[str, Any]]:
    extraction_summaries = {}
    for run in build_extraction_runs(args):
        run.db_path.parent.mkdir(parents=True, exist_ok=True)
        command = [
            str(run.colmap_bin),
            "feature_extractor",
            "--database_path",
            str(run.db_path),
            *run.args,
        ]
        elapsed_seconds = run_command(
            f"extract:{run.label}", command, run.log_path
        )
        exact_cap = (
            apply_exact_feature_cap(run.db_path, args.max_num_features)
            if args.exact_feature_cap
            else {"row_cap": args.max_num_features, "disabled": 1}
        )
        extraction_summaries[run.label] = {
            "label": run.label,
            "backend": run.backend,
            "colmap_bin": str(run.colmap_bin),
            "db_path": str(run.db_path),
            "log_path": str(run.log_path),
            "extraction_seconds": elapsed_seconds,
            "exact_cap": exact_cap,
            "capacity_drops": parse_capacity_drops(run.log_path),
            "database_after_cap": summarize_database(run.db_path),
        }
    return extraction_summaries


def run_pipelines(
    args: argparse.Namespace, extraction_summaries: dict[str, dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    extraction_runs = build_extraction_runs(args)
    pipelines = build_pipeline_runs(args, extraction_runs)
    pipeline_summaries = {}
    for pipeline in pipelines:
        pipeline.sparse_dir.mkdir(parents=True, exist_ok=True)
        match_command = [
            str(pipeline.colmap_bin),
            "sequential_matcher",
            "--database_path",
            str(pipeline.db_path),
            *pipeline.match_args,
        ]
        match_seconds = run_command(
            f"match:{pipeline.label}", match_command, pipeline.match_log_path
        )
        mapper_command = [
            str(pipeline.colmap_bin),
            "mapper",
            "--database_path",
            str(pipeline.db_path),
            "--image_path",
            str(args.image_dir),
            "--output_path",
            str(pipeline.sparse_dir),
            "--Mapper.num_threads",
            str(args.mapper_threads),
        ]
        mapper_seconds = run_command(
            f"mapper:{pipeline.label}", mapper_command, pipeline.mapper_log_path
        )
        model_summary = analyze_sparse_models(
            pipeline.colmap_bin,
            pipeline.sparse_dir,
            args.run_dir / "logs",
            pipeline.label,
        )
        db_summary = summarize_database(pipeline.db_path)
        extraction = extraction_summaries[pipeline.extractor_label]
        pipeline_summaries[pipeline.label] = {
            "label": pipeline.label,
            "extractor_label": pipeline.extractor_label,
            "extractor_backend": extraction["backend"],
            "matcher_backend": pipeline.matcher_backend,
            "db_path": str(pipeline.db_path),
            "sparse_dir": str(pipeline.sparse_dir),
            "extraction_seconds": extraction["extraction_seconds"],
            "matching_seconds": match_seconds,
            "mapping_seconds": mapper_seconds,
            "exact_cap": extraction["exact_cap"],
            "capacity_drops": extraction["capacity_drops"],
            "database": db_summary,
            "sparse_models": model_summary,
        }
    return pipeline_summaries


def csv_value(value: Any) -> Any:
    if isinstance(value, list):
        return " ".join("" if item is None else str(item) for item in value)
    return value


def write_outputs(
    args: argparse.Namespace,
    extraction_summaries: dict[str, dict[str, Any]],
    pipeline_summaries: dict[str, dict[str, Any]],
) -> None:
    summary = {
        "config": {
            "colmap_bin": str(args.colmap_bin),
            "image_dir": str(args.image_dir),
            "image_list_path": (
                str(args.image_list_path)
                if args.image_list_path is not None
                else None
            ),
            "run_dir": str(args.run_dir),
            "single_cpu_threads": args.single_cpu_threads,
            "threaded_cpu_threads": args.threaded_cpu_threads,
            "metal_threads": args.metal_threads,
            "mapper_threads": args.mapper_threads,
            "matcher_threads": args.matcher_threads,
            "max_image_size": args.max_image_size,
            "max_num_features": args.max_num_features,
            "max_num_orientations": args.max_num_orientations,
            "overlap": args.overlap,
            "exact_feature_cap": args.exact_feature_cap,
            "include_cuda": args.include_cuda,
            "cuda_colmap_bin": (
                str(args.cuda_colmap_bin)
                if args.cuda_colmap_bin is not None
                else None
            ),
            "cuda_gpu_index": args.cuda_gpu_index,
        },
        "extractions": extraction_summaries,
        "pipelines": pipeline_summaries,
    }
    json_path = args.run_dir / "summary.json"
    json_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    rows = []
    for pipeline in pipeline_summaries.values():
        largest_model = pipeline["sparse_models"]["largest_model"] or {}
        rows.append(
            {
                "label": pipeline["label"],
                "extractor_backend": pipeline["extractor_backend"],
                "matcher_backend": pipeline["matcher_backend"],
                "extraction_seconds": f"{pipeline['extraction_seconds']:.3f}",
                "matching_seconds": f"{pipeline['matching_seconds']:.3f}",
                "mapping_seconds": f"{pipeline['mapping_seconds']:.3f}",
                **pipeline["database"],
                "model_count": pipeline["sparse_models"]["model_count"],
                "registered_images_by_model": csv_value(
                    pipeline["sparse_models"]["registered_images_by_model"]
                ),
                "largest_registered_images": largest_model.get(
                    "registered_images"
                ),
                "largest_sparse_points": largest_model.get("sparse_points"),
                "largest_observations": largest_model.get("observations"),
                "largest_mean_reprojection_error_px": largest_model.get(
                    "mean_reprojection_error_px"
                ),
                "exact_cap_keypoint_rows_dropped": pipeline["exact_cap"].get(
                    "keypoint_rows_dropped"
                ),
                "metal_capacity_rows_dropped": pipeline["capacity_drops"].get(
                    "total_capacity_dropped"
                ),
                "metal_limit_rows_dropped": pipeline["capacity_drops"].get(
                    "total_limit_dropped"
                ),
                "metal_logged_rows_dropped": pipeline["capacity_drops"].get(
                    "total_dropped"
                ),
            }
        )

    csv_path = args.run_dir / "summary.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    print(f"\nSummary JSON: {json_path}")
    print(f"Summary CSV: {csv_path}")
    print("\nThroughput and reconstruction summary:")
    for row in rows:
        print(
            "{label}: extract={extraction_seconds}s match={matching_seconds}s "
            "models={model_count} reg_images={largest_registered_images} "
            "points={largest_sparse_points} observations={largest_observations} "
            "mean_reproj={largest_mean_reprojection_error_px} "
            "logged_drops={metal_logged_rows_dropped}".format(**row)
        )


def main() -> None:
    args = parse_args()
    check_input_paths(args)
    args.run_dir.mkdir(parents=True, exist_ok=True)
    extraction_summaries = run_extractions(args)
    pipeline_summaries = run_pipelines(args, extraction_summaries)
    write_outputs(args, extraction_summaries, pipeline_summaries)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(f"Command failed with exit code {error.returncode}", file=sys.stderr)
        sys.exit(error.returncode)
