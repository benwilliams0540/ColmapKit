#!/usr/bin/env python3

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path


EXPECTED_PROPERTIES = [
    "x", "y", "z", "scale_0", "scale_1", "scale_2", "opacity",
    "rot_0", "rot_1", "rot_2", "rot_3", "f_dc_0", "f_dc_1", "f_dc_2",
]
SH_C0 = 0.28209479177387814


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_ply(path: Path) -> tuple[list[str], list[tuple[float, ...]]]:
    with path.open("rb") as stream:
        header_lines: list[str] = []
        while True:
            line = stream.readline()
            if not line:
                raise ValueError("PLY ended before end_header")
            decoded = line.decode("ascii").rstrip("\n")
            header_lines.append(decoded)
            if decoded == "end_header":
                break
        if header_lines[:2] != ["ply", "format binary_little_endian 1.0"]:
            raise ValueError("PLY is not binary little endian 1.0")
        vertex_lines = [line for line in header_lines if line.startswith("element vertex ")]
        if len(vertex_lines) != 1:
            raise ValueError("PLY must declare exactly one vertex element")
        count = int(vertex_lines[0].split()[2])
        properties = [line.split()[2] for line in header_lines if line.startswith("property ")]
        if properties != EXPECTED_PROPERTIES:
            raise ValueError(f"Unexpected PLY properties: {properties}")
        if not any(line == "comment sh_degree 0" for line in header_lines):
            raise ValueError("PLY does not declare SH degree 0")
        payload = stream.read()
    record = struct.Struct("<14f")
    if len(payload) != count * record.size:
        raise ValueError("PLY payload size does not match vertex count")
    return header_lines, [record.unpack_from(payload, i * record.size) for i in range(count)]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args()
    run_dir = args.run_dir
    pose_path = run_dir / "refined-poses.json"
    ply_path = run_dir / "init.ply"
    tracked_path = run_dir / "tracked-evidence.json"
    prior_path = run_dir / "prior-evidence.json"
    pose = json.loads(pose_path.read_text())
    tracked = json.loads(tracked_path.read_text())
    prior = json.loads(prior_path.read_text())
    _, vertices = parse_ply(ply_path)

    pose_digest = sha256(pose_path)
    ply_digest = sha256(ply_path)
    assert pose["schema"] == "colmapkit.refined-poses.v2"
    assert pose["coordinate_system"] == "arkit_world_meters"
    assert pose["pose_convention"] == "column_major_world_from_camera"
    assert len(pose["rgb_manifest_sha256"]) == 64
    assert tracked["refined_pose_sha256"] == pose_digest
    assert prior["input_pose_sha256"] == pose_digest
    assert prior["output_pose_sha256"] == pose_digest
    assert prior["rgb_manifest_sha256"] == pose["rgb_manifest_sha256"]
    assert prior["output_ply_sha256"] == ply_digest
    assert prior["variant"] == "D"
    assert prior["sh_degree"] == 0
    assert prior["depth_used"] is False
    assert prior["poses_frozen"] is True
    assert prior["output_gaussians"] == len(vertices)
    assert prior["densification_ratio"] > 1.0
    assert prior["provenance_rgb_correspondence"] > 0

    for image in pose["images"]:
        assert len(image["world_from_camera"]) == 16
        assert len(image["rgb_sha256"]) == 64
        assert all(math.isfinite(value) for value in image["world_from_camera"])

    for vertex in vertices:
        if not all(math.isfinite(value) for value in vertex):
            raise ValueError("PLY contains a non-finite value")
        tangent0, tangent1, normal = map(math.exp, vertex[3:6])
        if not tangent0 > normal or not tangent1 > normal:
            raise ValueError("PLY Gaussian is not surface-tangent anisotropic")
        quaternion_norm = math.sqrt(sum(value * value for value in vertex[7:11]))
        if abs(quaternion_norm - 1.0) > 1e-4:
            raise ValueError("PLY contains a non-unit wxyz quaternion")
        alpha = 1.0 / (1.0 + math.exp(-vertex[6]))
        if not 0.0 < alpha < 1.0:
            raise ValueError("PLY opacity is not a finite logit")
        rgb = [value * SH_C0 + 0.5 for value in vertex[11:14]]
        if not all(-1e-4 <= value <= 1.0001 for value in rgb):
            raise ValueError("PLY DC coefficient decodes outside RGB range")

    print(json.dumps({
        "status": "ok",
        "registered_images": tracked["registered_images"],
        "sparse_points": prior["sparse_input_points"],
        "output_gaussians": len(vertices),
        "densification_ratio": prior["densification_ratio"],
        "pose_sha256": pose_digest,
        "ply_sha256": ply_digest,
    }, sort_keys=True))


if __name__ == "__main__":
    main()
