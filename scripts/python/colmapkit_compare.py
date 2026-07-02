#!/usr/bin/env python3

import argparse
import json
import math
import random
import shutil
import sqlite3
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class DatabaseMetrics:
    exists: bool
    images: int = 0
    keypoints: int = 0
    descriptors: int = 0
    raw_pairs: int = 0
    raw_matches: int = 0
    verified_pairs: int = 0
    verified_inliers: int = 0


@dataclass(frozen=True)
class SparseMetrics:
    exists: bool
    num_models: int = 0
    largest_model_index: int = -1
    registered_images: int = 0
    sparse_points: int = 0
    observations: int = 0
    mean_reprojection_error: float = 0.0


@dataclass(frozen=True)
class RunMetrics:
    label: str
    database: DatabaseMetrics
    sparse: SparseMetrics
    elapsed_seconds: dict[str, float]
    runtime_warnings: list[str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run CLI COLMAP and ColmapKit on the same small image set and "
            "compare database and sparse reconstruction metrics."
        )
    )
    parser.add_argument("--colmap-bin", type=Path, required=True)
    parser.add_argument("--colmapkit-bin", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--image-dir", type=Path)
    parser.add_argument("--image-list-path", type=Path)
    parser.add_argument(
        "--generate-synthetic-fixture",
        action="store_true",
        help="Generate a deterministic 8-image fixture in the run directory.",
    )
    parser.add_argument("--fixture-seed", type=int, default=4)
    parser.add_argument("--fixture-image-count", type=int, default=8)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument("--focal-length", type=float, default=900.0)
    parser.add_argument("--camera-model", default="SIMPLE_PINHOLE")
    parser.add_argument("--camera-params")
    parser.add_argument(
        "--matcher", choices=("sequential", "exhaustive"), default="sequential"
    )
    parser.add_argument("--sequential-overlap", type=int, default=4)
    parser.add_argument("--max-image-size", type=int, default=1024)
    parser.add_argument("--num-threads", type=int, default=1)
    parser.add_argument("--mapper-min-num-matches", type=int, default=15)
    parser.add_argument("--mapper-min-model-size", type=int, default=3)
    parser.add_argument("--mapper-random-seed", type=int, default=0)
    parser.add_argument(
        "--use-metal-matching",
        action="store_true",
        help="Use CPU SIFT extraction plus Metal descriptor matching in both runs.",
    )
    parser.add_argument(
        "--allow-metal-fallback",
        action="store_true",
        help="Do not fail if a requested Metal matcher falls back to CPU.",
    )
    parser.add_argument("--min-registered-images", type=int, default=3)
    parser.add_argument("--max-registered-image-delta", type=int, default=1)
    parser.add_argument("--max-sparse-point-delta-ratio", type=float, default=0.35)
    parser.add_argument("--max-observation-delta-ratio", type=float, default=0.35)
    parser.add_argument("--max-reprojection-error-delta", type=float, default=0.5)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def check_args(args: argparse.Namespace) -> None:
    if not args.colmap_bin.exists():
        raise SystemExit(f"Missing COLMAP binary: {args.colmap_bin}")
    if not args.colmapkit_bin.exists():
        raise SystemExit(f"Missing ColmapKit sample binary: {args.colmapkit_bin}")
    if args.image_dir is None and not args.generate_synthetic_fixture:
        raise SystemExit("Pass --image-dir or --generate-synthetic-fixture.")
    if args.image_dir is not None and not args.image_dir.is_dir():
        raise SystemExit(f"Missing image directory: {args.image_dir}")
    if args.image_list_path is not None and not args.image_list_path.is_file():
        raise SystemExit(f"Missing image list: {args.image_list_path}")
    if args.run_dir.exists() and any(args.run_dir.iterdir()):
        if not args.force:
            raise SystemExit(f"Refusing to overwrite non-empty run dir: {args.run_dir}")
        shutil.rmtree(args.run_dir)


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
    if return_code != 0:
        raise subprocess.CalledProcessError(return_code, command)
    return elapsed_seconds


def normalize(v: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(sum(c * c for c in v))
    return tuple(c / length for c in v)


def subtract(
    lhs: tuple[float, float, float], rhs: tuple[float, float, float]
) -> tuple[float, float, float]:
    return (lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2])


def cross(
    lhs: tuple[float, float, float], rhs: tuple[float, float, float]
) -> tuple[float, float, float]:
    return (
        lhs[1] * rhs[2] - lhs[2] * rhs[1],
        lhs[2] * rhs[0] - lhs[0] * rhs[2],
        lhs[0] * rhs[1] - lhs[1] * rhs[0],
    )


def dot(lhs: tuple[float, float, float], rhs: tuple[float, float, float]) -> float:
    return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2]


def look_at(
    camera_center: tuple[float, float, float],
    target: tuple[float, float, float],
) -> tuple[tuple[float, float, float], ...]:
    z_axis = normalize(subtract(target, camera_center))
    x_axis = normalize(cross(z_axis, (0.0, 1.0, 0.0)))
    y_axis = cross(x_axis, z_axis)
    return (x_axis, y_axis, z_axis)


def draw_disc(
    pixels: bytearray,
    width: int,
    height: int,
    center_x: float,
    center_y: float,
    radius: float,
    value: int,
) -> None:
    min_x = max(0, int(center_x - radius - 1))
    max_x = min(width - 1, int(center_x + radius + 1))
    min_y = max(0, int(center_y - radius - 1))
    max_y = min(height - 1, int(center_y + radius + 1))
    radius_sq = radius * radius
    for y in range(min_y, max_y + 1):
        for x in range(min_x, max_x + 1):
            dx = x - center_x
            dy = y - center_y
            if dx * dx + dy * dy <= radius_sq:
                pixels[y * width + x] = value


def write_pgm(path: Path, width: int, height: int, pixels: bytearray) -> None:
    with path.open("wb") as image_file:
        image_file.write(f"P5\n{width} {height}\n255\n".encode("ascii"))
        image_file.write(pixels)


def generate_synthetic_fixture(args: argparse.Namespace) -> Path:
    rng = random.Random(args.fixture_seed)
    image_dir = args.run_dir / "fixture" / "images"
    image_dir.mkdir(parents=True, exist_ok=True)

    points: list[tuple[tuple[float, float, float], int, float]] = []
    for _ in range(3200):
        face = rng.choice(("front", "back", "left", "right", "mid"))
        if face == "front":
            x = rng.uniform(-1.6, 1.6)
            y = rng.uniform(-1.0, 1.0)
            z = rng.uniform(3.8, 4.05)
        elif face == "back":
            x = rng.uniform(-1.6, 1.6)
            y = rng.uniform(-1.0, 1.0)
            z = rng.uniform(5.2, 5.5)
        elif face == "left":
            x = rng.uniform(-1.6, -1.3)
            y = rng.uniform(-1.0, 1.0)
            z = rng.uniform(3.8, 5.5)
        elif face == "right":
            x = rng.uniform(1.3, 1.6)
            y = rng.uniform(-1.0, 1.0)
            z = rng.uniform(3.8, 5.5)
        else:
            x = rng.uniform(-1.3, 1.3)
            y = rng.uniform(-0.8, 0.8)
            z = rng.uniform(4.2, 5.0)
        points.append(((x, y, z), rng.randint(0, 80), rng.choice((2.0, 3.0, 4.0))))

    for index in range(args.fixture_image_count):
        fraction = 0.0 if args.fixture_image_count == 1 else index / (
            args.fixture_image_count - 1
        )
        camera_x = -0.65 + 1.3 * fraction
        camera_center = (camera_x, 0.04 * math.sin(index), 0.0)
        rotation = look_at(camera_center, (0.0, 0.0, 4.6))
        pixels = bytearray([245] * (args.width * args.height))

        projected: list[tuple[float, float, float, int, float]] = []
        for point, gray, radius in points:
            relative = subtract(point, camera_center)
            cam_x = dot(rotation[0], relative)
            cam_y = dot(rotation[1], relative)
            cam_z = dot(rotation[2], relative)
            if cam_z <= 0.1:
                continue
            image_x = args.focal_length * cam_x / cam_z + args.width / 2.0
            image_y = args.focal_length * cam_y / cam_z + args.height / 2.0
            if -20 <= image_x < args.width + 20 and -20 <= image_y < args.height + 20:
                projected.append((cam_z, image_x, image_y, gray, radius))

        for cam_z, image_x, image_y, gray, radius in sorted(projected, reverse=True):
            scaled_radius = radius * max(0.7, min(1.4, 4.5 / cam_z))
            draw_disc(pixels, args.width, args.height, image_x, image_y, scaled_radius, gray)

        write_pgm(image_dir / f"frame_{index:03d}.pgm", args.width, args.height, pixels)

    return image_dir


def image_reader_args(args: argparse.Namespace, image_dir: Path) -> list[str]:
    camera_params = args.camera_params or (
        f"{args.focal_length},{args.width / 2.0},{args.height / 2.0}"
    )
    command = [
        "--image_path",
        str(image_dir),
        "--ImageReader.single_camera",
        "1",
        "--ImageReader.camera_model",
        args.camera_model,
        "--ImageReader.camera_params",
        camera_params,
    ]
    if args.image_list_path is not None:
        command += ["--image_list_path", str(args.image_list_path)]
    return command


def run_cli_pipeline(args: argparse.Namespace, image_dir: Path) -> RunMetrics:
    run_dir = args.run_dir / "cli"
    sparse_dir = run_dir / "sparse"
    text_dir = run_dir / "sparse-text"
    log_dir = run_dir / "logs"
    database_path = run_dir / "database.db"
    sparse_dir.mkdir(parents=True, exist_ok=True)

    elapsed = {}
    feature_command = [
        str(args.colmap_bin),
        "feature_extractor",
        "--database_path",
        str(database_path),
        *image_reader_args(args, image_dir),
        "--FeatureExtraction.use_gpu",
        "0",
        "--FeatureExtraction.max_image_size",
        str(args.max_image_size),
        "--FeatureExtraction.num_threads",
        str(args.num_threads),
    ]
    elapsed["feature_extractor"] = run_command(
        "CLI feature_extractor", feature_command, log_dir / "feature_extractor.log"
    )

    matcher_command = [
        str(args.colmap_bin),
        f"{args.matcher}_matcher",
        "--database_path",
        str(database_path),
        "--FeatureMatching.use_gpu",
        "1" if args.use_metal_matching else "0",
        "--FeatureMatching.num_threads",
        str(args.num_threads),
    ]
    if args.use_metal_matching:
        matcher_command += ["--SiftMatching.use_metal", "1"]
    if args.matcher == "sequential":
        matcher_command += [
            "--SequentialMatching.overlap",
            str(args.sequential_overlap),
            "--SequentialMatching.quadratic_overlap",
            "1",
            "--SequentialMatching.loop_detection",
            "0",
        ]
    elapsed[f"{args.matcher}_matcher"] = run_command(
        f"CLI {args.matcher}_matcher", matcher_command, log_dir / "matcher.log"
    )

    mapper_command = [
        str(args.colmap_bin),
        "mapper",
        "--database_path",
        str(database_path),
        "--image_path",
        str(image_dir),
        "--output_path",
        str(sparse_dir),
        "--Mapper.min_num_matches",
        str(args.mapper_min_num_matches),
        "--Mapper.min_model_size",
        str(args.mapper_min_model_size),
        "--Mapper.num_threads",
        str(args.num_threads),
        "--Mapper.random_seed",
        str(args.mapper_random_seed),
    ]
    if args.image_list_path is not None:
        mapper_command += ["--Mapper.image_list_path", str(args.image_list_path)]
    elapsed["mapper"] = run_command("CLI mapper", mapper_command, log_dir / "mapper.log")

    convert_sparse_models(args.colmap_bin, sparse_dir, text_dir, log_dir, elapsed)

    return RunMetrics(
        label="cli",
        database=summarize_database(database_path),
        sparse=summarize_sparse_text(text_dir),
        elapsed_seconds=elapsed,
        runtime_warnings=collect_runtime_warnings(log_dir),
    )


def run_colmapkit_pipeline(args: argparse.Namespace, image_dir: Path) -> RunMetrics:
    run_dir = args.run_dir / "colmapkit"
    sparse_dir = run_dir / "sparse"
    text_dir = run_dir / "sparse-text"
    log_dir = run_dir / "logs"
    database_path = run_dir / "database.db"
    run_dir.mkdir(parents=True, exist_ok=True)

    camera_params = args.camera_params or (
        f"{args.focal_length},{args.width / 2.0},{args.height / 2.0}"
    )
    command = [
        str(args.colmapkit_bin),
        "--database_path",
        str(database_path),
        "--image_path",
        str(image_dir),
        "--output_path",
        str(sparse_dir),
        "--sparse_text_output_path",
        str(text_dir),
        "--camera_model",
        args.camera_model,
        "--camera_params",
        camera_params,
        "--single_camera",
        "1",
        "--max_image_size",
        str(args.max_image_size),
        "--num_threads",
        str(args.num_threads),
        "--matcher",
        args.matcher,
        "--sequential_overlap",
        str(args.sequential_overlap),
        "--mapper_min_num_matches",
        str(args.mapper_min_num_matches),
        "--mapper_min_model_size",
        str(args.mapper_min_model_size),
        "--mapper_random_seed",
        str(args.mapper_random_seed),
        "--write_sparse_text",
        "1",
        "--use_metal_matching",
        "1" if args.use_metal_matching else "0",
        "--use_metal_sift",
        "0",
    ]
    if args.image_list_path is not None:
        command += ["--image_list_path", str(args.image_list_path)]

    elapsed = {
        "colmapkit_sparse_reconstruct": run_command(
            "ColmapKit sparse_reconstruct", command, log_dir / "colmapkit.log"
        )
    }
    return RunMetrics(
        label="colmapkit",
        database=summarize_database(database_path),
        sparse=summarize_sparse_text(text_dir),
        elapsed_seconds=elapsed,
        runtime_warnings=collect_runtime_warnings(log_dir),
    )


def collect_runtime_warnings(log_dir: Path) -> list[str]:
    warning_lines = []
    if not log_dir.exists():
        return warning_lines
    patterns = ("falling back", "unavailable", "failed at runtime")
    for log_path in sorted(log_dir.glob("*.log")):
        for line in log_path.read_text(encoding="utf-8").splitlines():
            lower_line = line.lower()
            if any(pattern in lower_line for pattern in patterns):
                warning_lines.append(f"{log_path.name}: {line.strip()}")
    return warning_lines


def convert_sparse_models(
    colmap_bin: Path,
    sparse_dir: Path,
    text_dir: Path,
    log_dir: Path,
    elapsed: dict[str, float],
) -> None:
    text_dir.mkdir(parents=True, exist_ok=True)
    model_dirs = [
        path
        for path in sorted(sparse_dir.iterdir())
        if path.is_dir() and (path / "cameras.bin").exists()
    ]
    for model_dir in model_dirs:
        output_dir = text_dir / model_dir.name
        output_dir.mkdir(parents=True, exist_ok=True)
        command = [
            str(colmap_bin),
            "model_converter",
            "--input_path",
            str(model_dir),
            "--output_path",
            str(output_dir),
            "--output_type",
            "TXT",
        ]
        elapsed[f"model_converter_{model_dir.name}"] = run_command(
            f"CLI model_converter {model_dir.name}",
            command,
            log_dir / f"model_converter_{model_dir.name}.log",
        )


def summarize_database(database_path: Path) -> DatabaseMetrics:
    if not database_path.exists():
        return DatabaseMetrics(exists=False)

    con = sqlite3.connect(database_path)
    cur = con.cursor()
    metrics = DatabaseMetrics(
        exists=True,
        images=cur.execute("select count(*) from images").fetchone()[0],
        keypoints=cur.execute("select coalesce(sum(rows), 0) from keypoints").fetchone()[0],
        descriptors=cur.execute(
            "select coalesce(sum(rows), 0) from descriptors"
        ).fetchone()[0],
        raw_pairs=cur.execute("select count(*) from matches where rows > 0").fetchone()[0],
        raw_matches=cur.execute(
            "select coalesce(sum(rows), 0) from matches where rows > 0"
        ).fetchone()[0],
        verified_pairs=cur.execute(
            "select count(*) from two_view_geometries where rows > 0"
        ).fetchone()[0],
        verified_inliers=cur.execute(
            "select coalesce(sum(rows), 0) from two_view_geometries where rows > 0"
        ).fetchone()[0],
    )
    con.close()
    return metrics


def parse_component(component_dir: Path) -> SparseMetrics:
    images_path = component_dir / "images.txt"
    points_path = component_dir / "points3D.txt"
    if not images_path.exists() or not points_path.exists():
        return SparseMetrics(exists=False)

    image_lines = [
        line.strip()
        for line in images_path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.startswith("#")
    ]
    registered_images = len(image_lines) // 2
    observations = 0
    for points2d_line in image_lines[1::2]:
        values = points2d_line.split()
        for point3d_id in values[2::3]:
            if point3d_id != "-1":
                observations += 1

    point_lines = [
        line.strip()
        for line in points_path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.startswith("#")
    ]
    sparse_points = len(point_lines)
    error_sum = 0.0
    error_count = 0
    for line in point_lines:
        values = line.split()
        if len(values) >= 8:
            error_sum += float(values[7])
            error_count += 1

    return SparseMetrics(
        exists=True,
        num_models=1,
        largest_model_index=int(component_dir.name),
        registered_images=registered_images,
        sparse_points=sparse_points,
        observations=observations,
        mean_reprojection_error=error_sum / error_count if error_count else 0.0,
    )


def summarize_sparse_text(text_dir: Path) -> SparseMetrics:
    if not text_dir.exists():
        return SparseMetrics(exists=False)

    components = [
        parse_component(path)
        for path in sorted(text_dir.iterdir(), key=lambda p: int(p.name))
        if path.is_dir() and path.name.isdigit()
    ]
    components = [component for component in components if component.exists]
    if not components:
        return SparseMetrics(exists=False)

    largest = max(
        components,
        key=lambda component: (component.registered_images, component.sparse_points),
    )
    return SparseMetrics(
        exists=True,
        num_models=len(components),
        largest_model_index=largest.largest_model_index,
        registered_images=largest.registered_images,
        sparse_points=largest.sparse_points,
        observations=largest.observations,
        mean_reprojection_error=largest.mean_reprojection_error,
    )


def relative_delta(lhs: int, rhs: int) -> float:
    denominator = max(lhs, rhs, 1)
    return abs(lhs - rhs) / denominator


def compare_metrics(
    args: argparse.Namespace, cli: RunMetrics, colmapkit: RunMetrics
) -> list[str]:
    failures = []
    if not cli.database.exists:
        failures.append("CLI database was not created.")
    if not colmapkit.database.exists:
        failures.append("ColmapKit database was not created.")
    if not cli.sparse.exists:
        failures.append("CLI sparse text model was not created.")
    if not colmapkit.sparse.exists:
        failures.append("ColmapKit sparse text model was not created.")

    if cli.sparse.registered_images < args.min_registered_images:
        failures.append(
            "CLI registered image count below threshold: "
            f"{cli.sparse.registered_images} < {args.min_registered_images}."
        )
    if colmapkit.sparse.registered_images < args.min_registered_images:
        failures.append(
            "ColmapKit registered image count below threshold: "
            f"{colmapkit.sparse.registered_images} < {args.min_registered_images}."
        )

    registered_delta = abs(
        cli.sparse.registered_images - colmapkit.sparse.registered_images
    )
    if registered_delta > args.max_registered_image_delta:
        failures.append(
            "Registered image count delta too high: "
            f"{registered_delta} > {args.max_registered_image_delta}."
        )

    point_delta_ratio = relative_delta(
        cli.sparse.sparse_points, colmapkit.sparse.sparse_points
    )
    if point_delta_ratio > args.max_sparse_point_delta_ratio:
        failures.append(
            "Sparse point delta ratio too high: "
            f"{point_delta_ratio:.3f} > {args.max_sparse_point_delta_ratio:.3f}."
        )

    observation_delta_ratio = relative_delta(
        cli.sparse.observations, colmapkit.sparse.observations
    )
    if observation_delta_ratio > args.max_observation_delta_ratio:
        failures.append(
            "Observation delta ratio too high: "
            f"{observation_delta_ratio:.3f} > {args.max_observation_delta_ratio:.3f}."
        )

    reprojection_delta = abs(
        cli.sparse.mean_reprojection_error
        - colmapkit.sparse.mean_reprojection_error
    )
    if reprojection_delta > args.max_reprojection_error_delta:
        failures.append(
            "Mean reprojection error delta too high: "
            f"{reprojection_delta:.3f} > {args.max_reprojection_error_delta:.3f}."
        )

    if args.use_metal_matching and not args.allow_metal_fallback:
        fallback_warnings = [
            *cli.runtime_warnings,
            *colmapkit.runtime_warnings,
        ]
        if fallback_warnings:
            failures.append(
                "Requested Metal matching fell back or became unavailable. "
                "Re-run with --allow-metal-fallback only when intentionally "
                "checking fallback parity."
            )
    return failures


def write_report(
    args: argparse.Namespace,
    image_dir: Path,
    cli: RunMetrics,
    colmapkit: RunMetrics,
    failures: list[str],
) -> None:
    report = {
        "image_dir": str(image_dir),
        "passed": not failures,
        "failures": failures,
        "thresholds": {
            "min_registered_images": args.min_registered_images,
            "max_registered_image_delta": args.max_registered_image_delta,
            "max_sparse_point_delta_ratio": args.max_sparse_point_delta_ratio,
            "max_observation_delta_ratio": args.max_observation_delta_ratio,
            "max_reprojection_error_delta": args.max_reprojection_error_delta,
        },
        "options": {
            "matcher": args.matcher,
            "use_metal_matching": args.use_metal_matching,
            "allow_metal_fallback": args.allow_metal_fallback,
            "mapper_random_seed": args.mapper_random_seed,
            "synthetic_fixture": args.generate_synthetic_fixture,
        },
        "cli": asdict(cli),
        "colmapkit": asdict(colmapkit),
    }
    report_path = args.run_dir / "comparison-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"\nWrote {report_path}")
    print(json.dumps(report, indent=2))


def main() -> None:
    args = parse_args()
    check_args(args)
    args.run_dir.mkdir(parents=True, exist_ok=True)

    if args.generate_synthetic_fixture:
        image_dir = generate_synthetic_fixture(args)
    else:
        assert args.image_dir is not None
        image_dir = args.image_dir

    cli = run_cli_pipeline(args, image_dir)
    colmapkit = run_colmapkit_pipeline(args, image_dir)
    failures = compare_metrics(args, cli, colmapkit)
    write_report(args, image_dir, cli, colmapkit, failures)

    if failures:
        print("\nComparison failed:")
        for failure in failures:
            print(f"- {failure}")
        raise SystemExit(1)

    print("\nComparison passed.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(f"Command failed with exit code {error.returncode}: {error.cmd}", file=sys.stderr)
        raise SystemExit(error.returncode)
